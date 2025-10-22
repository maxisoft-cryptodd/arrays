# tests/test_convenience.py
"""
Tests for the high-level functions in cryptodd_arrays.convenience.
"""
import pytest
import numpy as np
from pathlib import Path

from cryptodd_arrays import save_array, load_array, Codec

def test_save_and_load_roundtrip_auto_codec(tmp_path: Path):
    """
    Tests a basic save/load cycle using automatic codec selection.
    `tmp_path` is a pytest fixture that provides a temporary directory.
    """
    # 1. Setup: Create data and define file path
    filepath = tmp_path / "test.cdd"
    original_data = np.arange(1000, dtype=np.int64)

    # 2. Action: Save the array
    save_array(str(filepath), original_data)

    # 3. Verification: Load the array and check for correctness
    assert filepath.exists()
    loaded_data = load_array(str(filepath))

    assert loaded_data.dtype == original_data.dtype
    assert loaded_data.shape == original_data.shape
    np.testing.assert_array_equal(loaded_data, original_data)


def test_save_and_load_with_explicit_codec_and_metadata(tmp_path: Path):
    """
    Tests saving with a specific codec and metadata, then loading.
    """
    # 1. Setup
    filepath = tmp_path / "test_meta.cdd"
    original_data = np.random.rand(10, 5).astype(np.float32)
    metadata = {"source": "test_script", "version": 1.2}

    # 2. Action
    save_array(
        str(filepath),
        original_data,
        codec=Codec.ZSTD_COMPRESSED,
        user_metadata=metadata,
        zstd_level=5 # A codec-specific parameter
    )

    # 3. Verification
    loaded_data = load_array(str(filepath))
    np.testing.assert_array_equal(loaded_data, original_data)

    # We can also verify metadata by dropping down to the Reader object
    from cryptodd_arrays import open as cdd_open
    with cdd_open(str(filepath)) as f:
        assert f.user_metadata == metadata

def test_load_array_fails_on_multi_chunk_file(tmp_path: Path):
    """
    Ensures `load_array` raises a ValueError for files with more than one chunk.
    """
    filepath = tmp_path / "multi_chunk.cdd"

    # Create a multi-chunk file using the full API
    from cryptodd_arrays import open as cdd_open
    with cdd_open(str(filepath), 'w') as f:
        f.append(np.array([1, 2, 3]))
        f.append(np.array([4, 5, 6]))

    # Use pytest.raises to assert that a specific exception is thrown
    with pytest.raises(ValueError, match="Expected 1 chunk .* but found 2"):
        load_array(str(filepath))
