# tests/conftest.py
"""
Pytest configuration and shared fixtures for the test suite.
"""
import pytest
from pathlib import Path
import numpy as np

from cryptodd_arrays import open as cdd_open, Codec
from cryptodd_arrays.stream import GroupedWriter

@pytest.fixture(scope="session")
def multi_chunk_file(tmp_path_factory) -> Path:
    """
    A pytest fixture that creates a standard multi-chunk .cdd file.
    This runs only once per test session and provides the file path to tests.
    """
    filepath = tmp_path_factory.getbasetemp() / "multi_chunk_standard.cdd"
    metadata = {"source": "pytest_fixture", "num_chunks": 3}

    with cdd_open(str(filepath), 'w', user_metadata=metadata) as f:
        # Chunk 0: INT64, RAW
        f.append_chunk(np.arange(10, dtype=np.int64), codec='RAW')
        # Chunk 1: FLOAT32, ZSTD
        f.append_chunk(np.linspace(0, 1, 5, dtype=np.float32), codec='ZSTD_COMPRESSED')
        # Chunk 2: INT64, RAW
        f.append_chunk(np.arange(10, 20, dtype=np.int64), codec='RAW')

    return filepath

@pytest.fixture(scope="session")
def grouped_file(tmp_path_factory) -> Path:
    """
    Creates a .cdd file with interleaved chunks using the GroupedWriter.
    The structure is: prices_0, volumes_0, prices_1, volumes_1.
    """
    filepath = tmp_path_factory.getbasetemp() / "grouped_standard.cdd"

    with cdd_open(str(filepath), 'w') as f:
        with GroupedWriter(f, target_chunk_bytes=1024) as grouper: # Small target to ensure flushing
            # Group 0
            grouper.append({
                "prices": np.linspace(100, 101, 50, dtype=np.float32).reshape(50, 1),
                "volumes": np.arange(1000, 1050, dtype=np.int64)
            })
            # Group 1
            grouper.append({
                "prices": np.linspace(101, 102, 50, dtype=np.float32).reshape(50, 1),
                "volumes": np.arange(1050, 1100, dtype=np.int64)
            })

    return filepath
