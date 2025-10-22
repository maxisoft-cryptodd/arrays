# tests/conftest.py
"""
Pytest configuration and shared fixtures for the test suite.
"""
import pytest
import numpy as np
from pathlib import Path

from cryptodd_arrays import open as cdd_open

@pytest.fixture(scope="session")
def multi_chunk_file(tmp_path_factory) -> Path:
    """
    A pytest fixture that creates a standard multi-chunk .cdd file.
    This runs only once per test session and provides the file path to tests.
    """
    # tmp_path_factory is used for session-scoped fixtures
    filepath = tmp_path_factory.getbasetemp() / "multi_chunk_standard.cdd"
    metadata = {"source": "pytest_fixture", "num_chunks": 3}
    
    with cdd_open(str(filepath), 'w', user_metadata=metadata) as f:
        # Chunk 0: int64
        f.append_chunk(np.arange(10, dtype=np.int64), codec='RAW')
        # Chunk 1: float32
        f.append_chunk(np.linspace(0, 1, 5, dtype=np.float32), codec='ZSTD_COMPRESSED')
        # Chunk 2: int64 (same type as chunk 0)
        f.append_chunk(np.arange(10, 20, dtype=np.int64), codec='RAW')

    return filepath
