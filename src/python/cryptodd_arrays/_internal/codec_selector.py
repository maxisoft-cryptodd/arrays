# cryptodd_arrays/_internal/codec_selector.py

"""
Internal logic for automatically recommending a codec for a given array.
"""

import numpy as np
from ..types import Codec

def recommend_codec(data: np.ndarray) -> Codec:
    """
    Recommends an optimal codec based on array shape and dtype.

    This implements the "magic" behind the `writer.append()` method, aiming for
    high compression and performance without requiring user expertise.

    The heuristics are:
    - 3D float32 arrays are treated as orderbook data.
    - 2D float32/int64 arrays are treated as generic temporal 2D data.
    - 1D float32/int64 arrays are treated as generic temporal 1D data.
    - All other arrays default to ZSTD for general-purpose compression.

    Args:
        data: The NumPy array to analyze.

    Returns:
        The recommended Codec enum member.
    """
    # Using Python 3.10+ structural pattern matching for a clean,
    # declarative implementation of the heuristics.
    match (data.ndim, data.dtype):
        case (3, np.dtype('float32')):
            return Codec.GENERIC_OB_SIMD_F32

        case (2, np.dtype('float32')):
            return Codec.TEMPORAL_2D_SIMD_F32
        case (2, np.dtype('int64')):
            return Codec.TEMPORAL_2D_SIMD_I64

        case (1, np.dtype('float32')):
            return Codec.TEMPORAL_1D_SIMD_F32_XOR_SHUFFLE
        case (1, np.dtype('int64')):
            return Codec.TEMPORAL_1D_SIMD_I64_DELTA

        # Default case for all other shapes and dtypes
        case _:
            return Codec.ZSTD_COMPRESSED
