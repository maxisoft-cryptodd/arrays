# cryptodd_arrays/types.py

"""
Core type-safe enumerations and dataclasses for the cryptodd_arrays library.
"""
from enum import IntEnum

class Codec(IntEnum):
    """
    Enumeration of all available data codecs for compressing chunks.

    These codecs correspond directly to the C++ backend's `ChunkDataType` enum.
    """
    # General Purpose
    RAW = 0
    ZSTD_COMPRESSED = 1

    # Orderbook (3D: time, side, features)
    GENERIC_OB_SIMD_F16_AS_F32 = 6
    GENERIC_OB_SIMD_F32 = 7

    # Temporal 1D (e.g., timestamp or price series)
    TEMPORAL_1D_SIMD_F16_XOR_SHUFFLE_AS_F32 = 8
    TEMPORAL_1D_SIMD_F32_XOR_SHUFFLE = 9
    TEMPORAL_1D_SIMD_I64_XOR = 10
    TEMPORAL_1D_SIMD_I64_DELTA = 11

    # Temporal 2D (e.g., time series of feature vectors)
    TEMPORAL_2D_SIMD_F16_AS_F32 = 12
    TEMPORAL_2D_SIMD_F32 = 13
    TEMPORAL_2D_SIMD_I64 = 14

    # Deprecated/Exchange-Specific (for reference)
    OKX_OB_SIMD_F16_AS_F32 = 2
    OKX_OB_SIMD_F32 = 3
    BINANCE_OB_SIMD_F16_AS_F32 = 4
    BINANCE_OB_SIMD_F32 = 5
