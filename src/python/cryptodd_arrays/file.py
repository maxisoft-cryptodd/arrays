# cryptodd_arrays/file.py
"""High-level Reader, Writer, and the main `open` factory function."""

import json
import base64
from typing import Any, Optional, overload, Union, List
import numpy as np
from functools import cached_property

from .abc import CddFileBase
from .lowlevel import LowLevelWrapper
from .types import Codec
from .dataclasses import ChunkInfo, FileHeaderInfo, StoreResult
from ._internal import json_builder, numpy_utils, codec_selector
from .exceptions import CddConfigError # Import needed for `open`


def open(
    path: str,
    mode: str = 'r',
    *,
    user_metadata: Optional[dict[str, Any]] = None,
    check_checksums: bool = True
) -> Union["Reader", "Writer"]:
    """
    Opens a cryptodd-arrays file for reading, writing, or appending.
    This function is the primary entry point for the library.

    Args:
        path (str): Path to the .cdd file.
        mode (str): 'r' (read-only), 'w' (write, truncates if exists),
                    'a' (append to existing or create new).
        user_metadata (dict, optional): For 'w' mode only. Sets the
            file-level metadata upon creation. Must be JSON-serializable.
        check_checksums (bool): For 'r' mode only. If True (default),
            verifies data integrity on read.

    Returns:
        A Reader or Writer object, typically used within a `with` statement.

    Raises:
        CddConfigError: If the configuration is invalid (e.g., bad path).
        ValueError: If mode or arguments are invalid.
    """
    backend_config: dict[str, Any]
    writer_options: dict[str, Any] = {}

    if mode == 'r':
        if user_metadata is not None:
            raise ValueError("user_metadata can only be provided in 'w' mode.")
        backend_config = {"type": "File", "mode": "Read", "path": path}
    elif mode in ('w', 'a'):
        mode_str = "WriteTruncate" if mode == 'w' else "WriteAppend"
        backend_config = {"type": "File", "mode": mode_str, "path": path}
        if mode == 'w' and user_metadata is not None:
            try:
                json_str = json.dumps(user_metadata, separators=(',', ':'))
                b64_str = base64.b64encode(json_str.encode('utf-8')).decode('ascii')
                writer_options["user_metadata_base64"] = b64_str
            except TypeError as e:
                raise TypeError(f"user_metadata must be JSON-serializable. {e}") from e
        elif mode == 'a' and user_metadata is not None:
             raise ValueError("user_metadata cannot be provided in 'a' (append) mode.")
    else:
        raise ValueError(f"Unsupported mode: '{mode}'. Must be 'r', 'w', or 'a'.")

    full_config = {"backend": backend_config}
    if writer_options:
        full_config["writer_options"] = writer_options

    # We can't use json.dumps on the full_config here because LowLevelWrapper expects the config string.
    # So we create the JSON string here.
    try:
        json_config_str = json.dumps(full_config)
    except TypeError as e:
        raise CddConfigError(f"Failed to serialize configuration to JSON: {e}") from e


    wrapper = LowLevelWrapper(json_config_str)

    if mode == 'r':
        return Reader(wrapper, check_checksums=check_checksums)
    else:
        return Writer(wrapper)

# =============================================================================
# Writer and Reader classes remain the same as before
# =============================================================================

class Writer(CddFileBase):
    """
    A file handle for writing or appending to a .cdd file.
    Created via `cryptodd_arrays.open(..., mode='w'|'a')`.
    """
    def __init__(self, wrapper: LowLevelWrapper):
        self._wrapper = wrapper

    def append(self, data: np.ndarray, **codec_params: Any) -> StoreResult:
        """
        Appends a NumPy array as a new chunk, automatically selecting an
        optimal codec based on the data's shape and dtype.

        For explicit codec control, use `append_chunk()`.

        Args:
            data (np.ndarray): The data to write. Must be C-contiguous.
            **codec_params: Optional parameters for the chosen codec (e.g., zstd_level=5).

        Returns:
            A StoreResult object with details of the write operation.
        """
        numpy_utils.validate_array_for_writing(data)
        recommended_codec = codec_selector.recommend_codec(data)
        return self.append_chunk(data, recommended_codec, **codec_params)

    def append_chunk(
        self,
        data: np.ndarray,
        codec: Union[Codec, str],
        **codec_params: Any
    ) -> StoreResult:
        """
        Appends a NumPy array as a new chunk with an explicitly specified codec.

        Args:
            data (np.ndarray): The data to write. Must be C-contiguous.
            codec: The codec to use, either as a Codec enum or string name.
            **codec_params: Optional parameters for the codec (e.g., zstd_level).

        Returns:
            A StoreResult object with details of the write operation.
        """
        numpy_utils.validate_array_for_writing(data)
        req = json_builder.build_store_chunk_req(data, codec, codec_params)
        result = self._wrapper.execute(req, input_data=data)
        
        details = result.get("details", {})
        return StoreResult(
            chunk_index=details.get("chunk_index", -1),
            original_size=details.get("original_size", -1),
            compressed_size=details.get("compressed_size", -1),
            compression_ratio=details.get("compression_ratio", 0.0),
        )

    def flush(self) -> None:
        """Flushes any buffered data to the underlying storage."""
        req = json_builder.build_flush_req()
        self._wrapper.execute(req)

    def close(self) -> None:
        self.flush()
        self._wrapper.close()

    @property
    def closed(self) -> bool:
        return self._wrapper.closed


class Reader(CddFileBase):
    """
    A file handle for reading a .cdd file.
    Created via `cryptodd_arrays.open(..., mode='r')`.
    """
    def __init__(self, wrapper: LowLevelWrapper, check_checksums: bool = True):
        self._wrapper = wrapper
        self._check_checksums = check_checksums

    @cached_property
    def _inspection(self) -> dict[str, Any]:
        """Internal method to lazily inspect the file and cache the results."""
        req = json_builder.build_inspect_req()
        return self._wrapper.execute(req)

    @cached_property
    def file_header(self) -> FileHeaderInfo:
        """Returns metadata from the file's header."""
        header_data = self._inspection.get("file_header", {})
        return FileHeaderInfo(**header_data)

    @cached_property
    def user_metadata(self) -> dict[str, Any]:
        """
        The custom user metadata dictionary stored in the file.
        Returns an empty dict if no metadata is set.
        """
        b64_str = self.file_header.user_metadata_base64
        if not b64_str:
            return {}
        try:
            json_bytes = base64.b64decode(b64_str)
            return json.loads(json_bytes)
        except (ValueError, TypeError, json.JSONDecodeError):
            # If it's not valid JSON, return a dict with the raw value
            return {"_raw_base64": b64_str}

    @cached_property
    def chunks(self) -> List[ChunkInfo]:
        """A list of `ChunkInfo` objects describing each chunk in the file."""
        summaries = self._inspection.get("chunk_summaries", [])
        return [
            ChunkInfo(
                index=s["index"],
                shape=tuple(s["shape"]),
                dtype=s["dtype"],
                codec=Codec[s["codec"]],
                encoded_size_bytes=s["encoded_size_bytes"],
                decoded_size_bytes=s["decoded_size_bytes"],
            ) for s in summaries
        ]

    @property
    def nchunks(self) -> int:
        """The total number of chunks in the file."""
        return self._inspection.get("total_chunks", 0)

    def __len__(self) -> int:
        return self.nchunks

    @overload
    def __getitem__(self, key: int) -> np.ndarray: ...

    @overload
    def __getitem__(self, key: slice) -> np.ndarray: ...

    def __getitem__(self, key: Union[int, slice]) -> np.ndarray:
        """
        Reads data by chunk index or slice.

        - `reader[5]` reads the 6th chunk.
        - `reader[2:5]` reads chunks 2, 3, and 4 and concatenates them into
          a single NumPy array.
        """
        resolved_slice: slice
        is_single_item = isinstance(key, int)

        if is_single_item:
            resolved_index = key if key >= 0 else key + self.nchunks
            if not (0 <= resolved_index < self.nchunks):
                raise IndexError("Chunk index out of range")
            resolved_slice = slice(resolved_index, resolved_index + 1)
        elif isinstance(key, slice):
            start, stop, step = key.indices(self.nchunks)
            if step != 1:
                raise IndexError("Slicing with a step is not supported.")
            resolved_slice = slice(start, stop)
        else:
            raise TypeError(f"Index must be an integer or slice, not {type(key).__name__}")

        chunks_to_load = self.chunks[resolved_slice]
        if not chunks_to_load:
            # Return an empty array with a sensible default dtype if slice is empty
            return np.array([], dtype=np.uint8)

        # Check for consistent dtypes for concatenation
        first_dtype_str = chunks_to_load[0].dtype
        output_dtype = numpy_utils.cdd_str_to_numpy_dtype(first_dtype_str)
        if not all(c.dtype == first_dtype_str for c in chunks_to_load):
            raise TypeError("Cannot concatenate chunks with different dtypes.")

        # Pre-allocate the output buffer
        total_elements = sum(np.prod(c.shape) for c in chunks_to_load)
        output_buffer = np.empty(int(total_elements), dtype=output_dtype)

        # Build request and execute
        req = json_builder.build_load_chunks_req(resolved_slice, self._check_checksums)
        result = self._wrapper.execute(req, output_data=output_buffer)

        # Reshape the flat buffer to its final N-dimensional shape
        final_shape = tuple(result.get("final_shape", output_buffer.shape))
        return output_buffer.reshape(final_shape)

    def close(self) -> None:
        self._wrapper.close()

    @property
    def closed(self) -> bool:
        return self._wrapper.closed
