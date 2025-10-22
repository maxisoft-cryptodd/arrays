# cryptodd_arrays/__init__.py
"""
High-performance, compressed NumPy array storage.
"""
import base64
import json
from typing import Optional, Union, Any

from .file import Reader, Writer
from .types import Codec
from .dataclasses import ChunkInfo, StoreResult # DType is not used here, but ChunkInfo and StoreResult are
from .exceptions import CddError, CddOperationError, CddConfigError
from .lowlevel import LowLevelWrapper

__version__ = "0.0.1"

def open(
    path: str,
    mode: str = 'r',
    *,
    user_metadata: Optional[dict[str, Any]] = None,
    check_checksums: bool = True
) -> Union[Reader, Writer]:
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
                # Same serialization as in json_builder for consistency
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

    wrapper = LowLevelWrapper(json.dumps(full_config))

    if mode == 'r':
        return Reader(wrapper, check_checksums=check_checksums)
    else:
        return Writer(wrapper)


# Define what gets imported with 'from cryptodd_arrays import *'
__all__ = [
    'open',
    'Reader',
    'Writer',
    'Codec',
    'ChunkInfo',
    'StoreResult',
    'CddError',
    'CddOperationError',
    'CddConfigError',
    '__version__',
]
