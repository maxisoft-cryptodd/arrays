# tests/test_stream.py
"""
Tests for the advanced streaming and grouped writers/readers.
"""
import pytest
import numpy as np
from pathlib import Path
from collections import defaultdict

from cryptodd_arrays import open as cdd_open, Codec
from cryptodd_arrays.stream import BufferedAutoChunker, GroupedWriter, GroupedReader

# Helper function to simulate the flushing logic for robust testing
def predict_buffered_flushes(data_stream, target_chunk_bytes, min_compression_ratio):
    """Predicts how many chunks the BufferedAutoChunker SHOULD create."""
    # Simulate the estimation on the first piece of data
    first_arr = data_stream[0]
    with cdd_open(None, 'w') as mem_writer:
        sample_bytes = 512 * 1024
        bytes_per_row = first_arr.nbytes / first_arr.shape[0]
        rows = max(1, int(sample_bytes / bytes_per_row))
        res = mem_writer.append_chunk(first_arr[:rows], Codec.ZSTD_COMPRESSED, zstd_level=-2)
    
    # Apply the safety valve for min_compression_ratio
    ratio = max(min_compression_ratio, res.compressed_size / res.original_size)
    uncompressed_target = int(target_chunk_bytes / ratio)
    
    # Simulate the append loop
    flushes = 0
    buffered_bytes = 0
    for arr in data_stream:
        buffered_bytes += arr.nbytes
        if buffered_bytes >= uncompressed_target:
            flushes += 1
            buffered_bytes = 0
            
    if buffered_bytes > 0:
        flushes += 1 # Final flush on exit
        
    return flushes

@pytest.mark.parametrize("data_type, min_ratio", [
    ("compressible", 0.1),  # Test with highly compressible data
    ("random", 0.8),        # Test with noisy, less compressible data
])
def test_buffered_autochunker_logic(tmp_path: Path, data_type, min_ratio):
    """
    Tests the buffered writer's smart algorithm with different data types.
    """
    filepath = tmp_path / f"buffered_{data_type}.cdd"
    target_bytes = 4096  # 4KB target compressed size

    # Create a stream of 10 small arrays
    arr_template = np.arange(200, dtype=np.int64) # 1600 bytes each
    if data_type == "compressible":
        data_stream = [np.zeros_like(arr_template) + i for i in range(10)]
    else: # random
        rng = np.random.default_rng(seed=42)
        data_stream = [rng.integers(0, 1000, size=200, dtype=np.int64) for _ in range(10)]

    # Use our helper to predict the outcome
    expected_chunks = predict_buffered_flushes(data_stream, target_bytes, min_ratio)

    with cdd_open(str(filepath), 'w') as f:
        with BufferedAutoChunker(f, target_chunk_bytes=target_bytes, min_compression_ratio=min_ratio) as buffer:
            for arr in data_stream:
                buffer.append(arr)

    # Verify the number of chunks matches the prediction
    with cdd_open(str(filepath), 'r') as f:
        assert f.nchunks == expected_chunks
        
        # Also verify data integrity
        full_data = np.concatenate([f[i] for i in range(f.nchunks)])
        expected_data = np.concatenate(data_stream)
        np.testing.assert_array_equal(full_data, expected_data)

def test_grouped_writer_reader_roundtrip(tmp_path: Path):
    """Tests a full write/read cycle with the grouped classes."""
    filepath = tmp_path / "grouped_roundtrip.cdd"
    array_names = ["prices", "volumes"]
    
    # Create test data
    all_prices = np.linspace(100, 110, 200, dtype=np.float32).reshape(-1, 2) # 100 rows
    all_volumes = np.arange(5000, 5100, dtype=np.int64) # 100 rows
    
    with cdd_open(str(filepath), 'w') as f:
        # Use a small target size to ensure at least one flush
        with GroupedWriter(f, target_chunk_bytes=2048) as grouper:
            # Append in two batches
            grouper.append({
                "prices": all_prices[:50],
                "volumes": all_volumes[:50]
            })
            grouper.append({
                "prices": all_prices[50:],
                "volumes": all_volumes[50:]
            })
            
    # Now read it back
    with cdd_open(str(filepath), 'r') as f:
        grouped_reader = GroupedReader(f, names=array_names)
        
        # The number of groups depends on the flushing logic, but should be at least 1
        assert grouped_reader.num_groups >= 1
        assert len(grouped_reader) >= 1
        
        # Read by iterating
        read_data = defaultdict(list)
        for group in grouped_reader:
            assert list(group.keys()) == array_names
            read_data["prices"].append(group["prices"])
            read_data["volumes"].append(group["volumes"])
            
        # Verify concatenated data
        final_prices = np.concatenate(read_data["prices"])
        final_volumes = np.concatenate(read_data["volumes"])
        
        np.testing.assert_allclose(final_prices, all_prices)
        np.testing.assert_array_equal(final_volumes, all_volumes)
        
        # Test random access
        # This test is now less specific about chunk boundaries due to dynamic flushing
        # We can still check if the first group is correct if it's a single chunk
        if grouped_reader.num_groups == 2: # If it happened to flush into two groups
            group_1 = grouped_reader[1]
            np.testing.assert_allclose(group_1["prices"], all_prices[50:])
            np.testing.assert_array_equal(group_1["volumes"], all_volumes[50:])

def test_grouped_reader_validation(tmp_path: Path):
    """Ensures the GroupedReader raises errors on misaligned files."""
    filepath = tmp_path / "misaligned.cdd"
    
    # Create a file with 3 chunks (not divisible by 2)
    with cdd_open(str(filepath), 'w') as f:
        f.append(np.arange(10))
        f.append(np.arange(10))
        f.append(np.arange(10))
        
    with cdd_open(str(filepath), 'r') as f:
        with pytest.raises(ValueError, match="not a multiple"):
            _ = GroupedReader(f, names=["a", "b"])
