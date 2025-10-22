# cryptodd_arrays/__init__.py
"""
High-performance, compressed NumPy array storage.
"""
# Public API is defined by imports from implementation modules
from .file import open, Reader, Writer
from .types import Codec
from .dataclasses import ChunkInfo, FileHeaderInfo, StoreResult
from .exceptions import CddError, CddOperationError, CddConfigError
from .convenience import save_array, load_array

__version__ = "0.0.1"

# Define what gets imported with 'from cryptodd_arrays import *'
__all__ = [
    'open',
    'Reader',
    'Writer',
    'save_array',
    'load_array',
    'Codec',
    'ChunkInfo',
    'FileHeaderInfo',
    'StoreResult',
    'CddError',
    'CddOperationError',
    'CddConfigError',
    '__version__',
]
