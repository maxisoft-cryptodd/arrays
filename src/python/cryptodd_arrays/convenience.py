# cryptodd_arrays/convenience.py
"""
High-level convenience functions for common single-array operations.
"""
from typing import Any, Optional
import numpy as np

from . import open as cdd_open
from .types import Codec

def save_array(
    filepath: str,
    data: np.ndarray,
    *,
    codec: Optional[Codec | str] = None,
    user_metadata: Optional[dict[str, Any]] = None,
    **codec_params: Any
) -> None:
    """
    Saves a single NumPy array to a new .cdd file.

    This is a high-level wrapper for the most common write operation.

    Args:
        filepath: The path to the file to be created.
        data: The NumPy array to save.
        codec: (Optional) The codec to use. If None, an optimal codec is
               chosen automatically.
        user_metadata: (Optional) A JSON-serializable dictionary to store as
                       file-level metadata.
        **codec_params: Optional parameters for the codec (e.g., zstd_level).
    """
    with cdd_open(filepath, mode='w', user_metadata=user_metadata) as f:
        if codec is None:
            f.append(data, **codec_params)
        else:
            f.append_chunk(data, codec, **codec_params)


def load_array(filepath: str, *, check_checksums: bool = True) -> np.ndarray:
    """
    Loads an array from a .cdd file that contains a single chunk.

    This is a high-level wrapper for the most common read operation.

    Args:
        filepath: The path to the .cdd file.
        check_checksums: If True (default), verifies data integrity on read.

    Returns:
        The loaded NumPy array.

    Raises:
        ValueError: If the file contains zero or more than one chunk.
    """
    with cdd_open(filepath, mode='r', check_checksums=check_checksums) as f:
        if f.nchunks != 1:
            raise ValueError(
                f"Expected 1 chunk in the file but found {f.nchunks}. "
                "Use `cryptodd_arrays.open()` for multi-chunk files."
            )
        return f[0]
