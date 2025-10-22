# cryptodd_arrays/dataclasses.py
"""
Dataclasses for structured data within the cryptodd_arrays library.
"""
from dataclasses import dataclass
from typing import Any, List, Tuple, Optional
from .types import Codec

@dataclass(frozen=True, slots=True)
class ChunkInfo:
    """A summary of a single chunk within a file."""
    index: int
    shape: Tuple[int, ...]
    dtype: str # String representation like 'FLOAT32'
    codec: Codec
    encoded_size_bytes: int
    decoded_size_bytes: int

@dataclass(frozen=True, slots=True)
class StoreResult:
    """Details of a write operation."""
    chunk_index: int
    original_size: int
    compressed_size: int
    compression_ratio: float

@dataclass(frozen=True, slots=True)
class FileHeaderInfo:
    """Information extracted from the file header."""
    version: int
    index_block_offset: int
    index_block_size: int
    user_metadata_base64: str
