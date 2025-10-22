# cryptodd_arrays/_internal/numpy_utils.py

"""
Internal utilities for interacting with NumPy arrays.

This module handles validation, and conversion between NumPy's data types/shapes
and the string representations required by the C-API.
"""

from typing import Any, TypeAlias
import numpy as np

# TypeAlias for clarity in function signatures.
DataSpecDict: TypeAlias = dict[str, str | list[int]]

# --- Mappings ---

# Maps NumPy dtype objects to the C-API string identifiers.
_NP_DTYPE_TO_CDD_STR: dict[np.dtype, str] = {
    np.dtype('float16'): "FLOAT16",
    np.dtype('float32'): "FLOAT32",
    np.dtype('float64'): "FLOAT64",
    np.dtype('int8'): "INT8",
    np.dtype('uint8'): "UINT8",
    np.dtype('int16'): "INT16",
    np.dtype('uint16'): "UINT16",
    np.dtype('int32'): "INT32",
    np.dtype('uint32'): "UINT32",
    np.dtype('int64'): "INT64",
    np.dtype('uint64'): "UINT64",
}

# Maps C-API string identifiers back to NumPy dtype objects.
_CDD_STR_TO_NP_DTYPE: dict[str, np.dtype] = {
    v: k for k, v in _NP_DTYPE_TO_CDD_STR.items()
}

# --- Functions ---

def validate_array_for_writing(arr: np.ndarray) -> None:
    """
    Ensures a NumPy array is suitable for passing to the C++ backend.

    The backend expects C-contiguous memory layouts for zero-copy operations.

    Args:
        arr: The NumPy array to validate.

    Raises:
        TypeError: If the array's dtype is not supported.
        ValueError: If the array is not C-contiguous.
    """
    if arr.dtype not in _NP_DTYPE_TO_CDD_STR:
        supported_types = ", ".join(dtype.name for dtype in _NP_DTYPE_TO_CDD_STR)
        raise TypeError(
            f"Unsupported NumPy dtype: '{arr.dtype.name}'. "
            f"Supported types are: {supported_types}"
        )

    if not arr.flags['C_CONTIGUOUS']:
        raise ValueError(
            "Array must be C-contiguous. Please call `np.ascontiguousarray(arr)` "
            "on your array before writing."
        )

def get_dataspec(arr: np.ndarray) -> DataSpecDict:
    """
    Generates the `data_spec` dictionary for an array.

    Args:
        arr: A NumPy array.

    Returns:
        A dictionary formatted for the C-API `data_spec` field.

    Raises:
        TypeError: If the array's dtype is not supported.
    """
    try:
        dtype_str = _NP_DTYPE_TO_CDD_STR[arr.dtype]
    except KeyError:
        supported_types = ", ".join(dtype.name for dtype in _NP_DTYPE_TO_CDD_STR)
        raise TypeError(
            f"Unsupported NumPy dtype: '{arr.dtype.name}'. "
            f"Supported types are: {supported_types}"
        ) from None

    return {
        "dtype": dtype_str,
        "shape": list(arr.shape),
    }

def cdd_str_to_numpy_dtype(dtype_str: str) -> np.dtype:
    """
    Converts a C-API dtype string to a NumPy dtype object.

    Args:
        dtype_str: The string identifier (e.g., "FLOAT32").

    Returns:
        The corresponding NumPy dtype.

    Raises:
        ValueError: If the dtype string is unknown.
    """
    try:
        return _CDD_STR_TO_NP_DTYPE[dtype_str]
    except KeyError:
        raise ValueError(f"Unknown C-API dtype string: '{dtype_str}'") from None
