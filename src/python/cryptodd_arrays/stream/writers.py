# cryptodd_arrays/stream/writers.py
"""
Advanced, high-level writer classes for streaming and grouped data.
"""
from collections import defaultdict
from typing import Any, Dict, List, Optional, Union, Set
import numpy as np

from ..file import Writer, open as cdd_open
from ..types import Codec
from ..dataclasses import StoreResult
from .._internal import codec_selector

class BufferedAutoChunker:
    """
    A writer that automatically buffers and chunks data for a single array stream.
    """
    def __init__(
            self,
            writer: Writer,
            *,
            target_chunk_bytes: int = 4 * 1024 * 1024,
            codec: Optional[Union[Codec, str]] = None,
            min_compression_ratio: float = 0.1,
            max_buffer_multiplier: int = 10,
            **codec_params: Any
    ):
        if not isinstance(writer, Writer):
            raise TypeError("writer must be a cryptodd_arrays.Writer object.")
        self.writer = writer
        self.target_chunk_bytes = target_chunk_bytes
        self.codec = codec
        self.codec_params = codec_params
        self.min_compression_ratio = min_compression_ratio
        self._max_buffer_bytes = target_chunk_bytes * max_buffer_multiplier

        self._buffer: List[np.ndarray] = []
        self._buffered_bytes: int = 0
        self._compression_ratio: Optional[float] = None

        # CHANGED: Better initial estimate based on user suggestion.
        initial_target = int(self.target_chunk_bytes / self.min_compression_ratio)
        self._uncompressed_chunk_size: int = min(initial_target, self._max_buffer_bytes)

    def _estimate_compression_ratio(self, data: np.ndarray) -> None:
        """Lazily estimates the compression ratio using a small sample."""
        if self._compression_ratio is not None or data.shape[0] == 0:
            return

        bytes_per_row = data.nbytes / data.shape[0]
        num_rows_to_sample = max(1, int((512 * 1024) / bytes_per_row))
        sample = data[:num_rows_to_sample]

        codec_to_use = self.codec or codec_selector.recommend_codec(sample)

        with cdd_open(None, 'w') as mem_writer:
            assert isinstance(mem_writer, Writer)
            result = mem_writer.append_chunk(sample, codec_to_use, zstd_level=-2)

        if result.original_size > 0:
            ratio = result.compressed_size / result.original_size
            self._compression_ratio = max(self.min_compression_ratio, ratio)
        else:
            self._compression_ratio = 0.5

        uncompressed_target = int(self.target_chunk_bytes / self._compression_ratio)
        self._uncompressed_chunk_size = min(uncompressed_target, self._max_buffer_bytes)

    def append(self, data: np.ndarray) -> None:
        """Appends an array to the buffer, flushing to disk if the buffer is full."""
        if self.writer.closed:
            raise ValueError("Cannot append to a closed writer.")

        if not self._buffer and self.codec is None:
            self.codec = codec_selector.recommend_codec(data)

        if self._compression_ratio is None:
            self._estimate_compression_ratio(data)

        self._buffer.append(data)
        self._buffered_bytes += data.nbytes

        if self._buffered_bytes >= self._uncompressed_chunk_size:
            self.flush()

    def flush(self) -> Optional[StoreResult]:
        if not self._buffer: return None
        full_chunk = np.concatenate(self._buffer)
        result = self.writer.append_chunk(full_chunk, codec=self.codec, **self.codec_params)
        self._buffer.clear()
        self._buffered_bytes = 0
        return result

    def close(self) -> None:
        self.flush()

    def __enter__(self) -> "BufferedAutoChunker": return self
    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        if exc_type is None:
            self.flush()

class GroupedWriter:
    """
    A writer for synchronous, time-aligned arrays (e.g., prices, volumes).
    """
    def __init__(
            self,
            writer: Writer,
            *,
            target_chunk_bytes: int = 4 * 1024 * 1024,
            codecs: Optional[Dict[str, Union[Codec, str]]] = None,
            codec_params: Optional[Dict[str, Dict[str, Any]]] = None,
            min_compression_ratio: float = 0.1,
            max_buffer_multiplier: int = 10,
    ):
        # CHANGED: Docstring restored and improved.
        """
        Initializes the grouped writer.

        Args:
            writer: An opened `cryptodd_arrays.Writer` instance.
            target_chunk_bytes: The desired total compressed size for one chunk
                                of *all* arrays combined.
            codecs: (Optional) A dictionary mapping array names to codecs.
                    If not provided for an array, it will be auto-selected.
            codec_params: (Optional) A nested dictionary mapping array names
                          to their specific codec parameters.
            min_compression_ratio: The most optimistic compression ratio to assume
                                   when estimating buffer size.
            max_buffer_multiplier: The maximum allowed size of the uncompressed
                                   buffer, as a multiple of `target_chunk_bytes`.
        """
        self.writer = writer
        self.target_chunk_bytes = target_chunk_bytes
        self.codecs = codecs or {}
        self.codec_params = codec_params or {}
        self.min_compression_ratio = min_compression_ratio
        self._max_buffer_bytes = target_chunk_bytes * max_buffer_multiplier

        self._buffers = defaultdict(list)
        self._total_buffered_bytes: int = 0

        # CHANGED: `_ratios` is no longer stored as persistent state.
        # CHANGED: Better initial estimate.
        initial_target = int(self.target_chunk_bytes / self.min_compression_ratio)
        self._uncompressed_chunk_size: int = min(initial_target, self._max_buffer_bytes)

        # CHANGED: State for tracking array names for validation.
        self._array_names: Optional[Set[str]] = None

    def _estimate_uncompressed_chunk_size(self, data: Dict[str, np.ndarray]) -> None:
        """Lazily estimates an overall uncompressed chunk size for the group."""
        if self._array_names is not None: return # Already estimated

        total_sample_original_bytes = 0.0
        total_sample_compressed_bytes = 0.0

        with cdd_open(None, 'w') as mem_writer:
            assert isinstance(mem_writer, Writer)
            for name, array in data.items():
                if array.shape[0] == 0: continue

                bytes_per_row = array.nbytes / array.shape[0]
                num_rows_to_sample = max(1, int((512 * 1024) / bytes_per_row))
                sample = array[:num_rows_to_sample]

                codec_to_use = self.codecs.get(name) or codec_selector.recommend_codec(sample)
                res = mem_writer.append_chunk(sample, codec_to_use, zstd_level=-2)

                total_sample_original_bytes += res.original_size
                total_sample_compressed_bytes += res.compressed_size

        if total_sample_original_bytes > 0:
            overall_ratio = total_sample_compressed_bytes / total_sample_original_bytes
            overall_ratio = max(self.min_compression_ratio, overall_ratio)
        else:
            overall_ratio = 0.5

        uncompressed_target = int(self.target_chunk_bytes / overall_ratio)
        self._uncompressed_chunk_size = min(uncompressed_target, self._max_buffer_bytes)

    def append(self, data: Dict[str, np.ndarray]) -> None:
        """Appends a dictionary of aligned arrays."""
        if self.writer.closed: raise ValueError("Cannot append to a closed writer.")
        if not data: return

        current_keys = set(data.keys())
        if self._array_names is None:
            # First append call: estimate sizes and store the expected set of keys.
            self._estimate_uncompressed_chunk_size(data)
            self._array_names = current_keys
        # CHANGED: More informative error message.
        elif self._array_names != current_keys:
            missing = self._array_names - current_keys
            extra = current_keys - self._array_names
            raise ValueError(
                "Appended group must contain the same set of array names. "
                f"Missing keys: {missing or 'None'}. "
                f"Extra keys: {extra or 'None'}."
            )

        num_rows = data[next(iter(data))].shape[0]
        if not all(arr.shape[0] == num_rows for arr in data.values()):
            raise ValueError("All arrays in a group append must have the same number of rows.")

        for name, array in data.items():
            self._buffers[name].append(array)
            self._total_buffered_bytes += array.nbytes

        if self._total_buffered_bytes >= self._uncompressed_chunk_size:
            self.flush()

    def flush(self) -> None:
        """Writes the current contents of all buffers to disk as aligned chunks."""
        if self._total_buffered_bytes == 0:
            return

        for name, array_list in self._buffers.items():
            if array_list:
                full_chunk = np.concatenate(array_list)
                codec_to_use = self.codecs.get(name) or codec_selector.recommend_codec(full_chunk)
                params_for_name = self.codec_params.get(name, {})
                self.writer.append_chunk(full_chunk, codec=codec_to_use, **params_for_name)

        self._buffers.clear()
        self._total_buffered_bytes = 0

    def close(self) -> None:
        self.flush()

    def __enter__(self) -> "GroupedWriter":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        if exc_type is None:
            self.flush()