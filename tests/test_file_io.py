# tests/test_file_io.py
"""
Comprehensive tests for the core Reader and Writer classes.
"""
import pytest
import numpy as np
from pathlib import Path

from cryptodd_arrays import open as cdd_open, CddError, Codec

# --- Writer Tests ---

def test_writer_create_and_append(tmp_path: Path):
    """Tests creating a file, appending chunks, and flushing."""
    filepath = tmp_path / "writer_test.cdd"
    
    with cdd_open(str(filepath), 'w') as f:
        assert not f.closed
        res1 = f.append(np.array([1, 2, 3], dtype=np.int32))
        res2 = f.append_chunk(np.array([4., 5., 6.], dtype=np.float32), 'RAW')
        
        assert res1.chunk_index == 0
        assert res2.chunk_index == 1
    
    assert f.closed
    assert filepath.exists()
    
    # Verify content with a reader
    with cdd_open(str(filepath), 'r') as f:
        assert f.nchunks == 2
        np.testing.assert_array_equal(f[0], np.array([1, 2, 3], dtype=np.int32))
        np.testing.assert_array_equal(f[1], np.array([4., 5., 6.], dtype=np.float32))

def test_writer_fails_on_non_contiguous_array(tmp_path: Path):
    """Ensures the C-contiguity check is working from the Python side."""
    filepath = tmp_path / "contig_test.cdd"
    non_contiguous_arr = np.zeros((5, 5), dtype=np.float32).T # Transpose creates a non-C-contiguous view
    
    with cdd_open(str(filepath), 'w') as f:
        with pytest.raises(ValueError, match="Array must be C-contiguous"):
            f.append(non_contiguous_arr)

def test_operation_on_closed_writer_fails(tmp_path: Path):
    filepath = tmp_path / "closed_test.cdd"
    f = cdd_open(str(filepath), 'w')
    f.close()
    assert f.closed
    with pytest.raises(ValueError, match="Operation attempted on a closed CddFile"):
        f.append(np.arange(5))

# --- Reader Tests (using the fixture) ---

def test_reader_properties(multi_chunk_file: Path):
    """Tests the lazy-loaded properties of the Reader."""
    with cdd_open(str(multi_chunk_file), 'r') as f:
        assert not f.closed
        assert f.nchunks == 3
        assert len(f) == 3
        assert f.user_metadata == {"source": "pytest_fixture", "num_chunks": 3}
        
        # Check chunks property
        chunks = f.chunks
        assert len(chunks) == 3
        assert chunks[0].index == 0
        assert chunks[0].shape == (10,)
        assert chunks[0].dtype == "INT64"
        assert chunks[1].codec == Codec.ZSTD_COMPRESSED

    assert f.closed

def test_reader_integer_indexing(multi_chunk_file: Path):
    """Tests reading single chunks with positive and negative indices."""
    with cdd_open(str(multi_chunk_file), 'r') as f:
        # Positive index
        chunk_1 = f[1]
        assert chunk_1.dtype == np.float32
        np.testing.assert_allclose(chunk_1, np.linspace(0, 1, 5, dtype=np.float32))
        
        # Negative index
        chunk_2 = f[-1]
        assert chunk_2.dtype == np.int64
        np.testing.assert_array_equal(chunk_2, np.arange(10, 20, dtype=np.int64))
        
        # Out of bounds
        with pytest.raises(IndexError):
            _ = f[99]
        with pytest.raises(IndexError):
            _ = f[-99]

def test_reader_slicing_and_concatenation(multi_chunk_file: Path, tmp_path: Path):
    """Tests reading slices of chunks, which triggers concatenation."""
    with cdd_open(str(multi_chunk_file), 'r') as f:
        # Slice [0:2] should fail due to different dtypes
        with pytest.raises(TypeError, match="Cannot concatenate chunks with different dtypes"):
            _ = f[0:2]
            
        # Slice [0:3:2] (or any step != 1) should fail
        with pytest.raises(IndexError, match="Slicing with a step is not supported"):
            _ = f[0:3:2]

    # New test case for valid concatenation
    concat_path = tmp_path / "concat_test.cdd"
    with cdd_open(str(concat_path), 'w') as f_w:
        f_w.append(np.arange(5, dtype=np.int16))
        f_w.append(np.arange(5, 10, dtype=np.int16))
        f_w.append(np.arange(10, 15, dtype=np.int16))

    with cdd_open(str(concat_path), 'r') as f_r:
        # Test open-ended slice
        loaded_slice_open = f_r[1:]
        np.testing.assert_array_equal(loaded_slice_open, np.arange(5, 15, dtype=np.int16))
        
        # Test closed slice
        loaded_slice_closed = f_r[0:2]
        np.testing.assert_array_equal(loaded_slice_closed, np.arange(10, dtype=np.int16))
