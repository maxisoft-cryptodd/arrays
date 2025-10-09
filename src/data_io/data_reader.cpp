#include "data_reader.h"

#include "blake3_stream_hasher.h"

#include <filesystem> // For std::filesystem::exists
#include <span>

namespace cryptodd {

// Helper to decompress data using Zstd
std::vector<uint8_t> DataReader::decompress_zstd(std::span<const uint8_t> input, size_t decompressed_size) {
    if (input.empty()) {
        return {};
    }
    std::vector<uint8_t> decompressed_data(decompressed_size);
    size_t const actual_decompressed_size = ZSTD_decompress(decompressed_data.data(), decompressed_size, input.data(), input.size());
    if (ZSTD_isError(actual_decompressed_size)) {
        throw std::runtime_error("Zstd decompression failed: " + std::string(ZSTD_getErrorName(actual_decompressed_size)));
    }
    if (actual_decompressed_size != decompressed_size) {
        throw std::runtime_error("Zstd decompression size mismatch. Expected " + std::to_string(decompressed_size) + ", got " + std::to_string(actual_decompressed_size));
    }
    return decompressed_data;
}

DataReader DataReader::open(const std::filesystem::path& filepath) {
    if (!std::filesystem::exists(filepath)) {
        throw std::runtime_error("File does not exist: " + filepath.string());
    }
    auto backend = std::make_unique<FileBackend>(filepath, std::ios_base::in | std::ios_base::binary);
    return DataReader(std::move(backend));
}

DataReader DataReader::open_in_memory(std::unique_ptr<IStorageBackend> backend) {
    return DataReader(std::move(backend));
}

DataReader::DataReader(std::unique_ptr<IStorageBackend> backend) : backend_(std::move(backend)) {
    // Read FileHeader
    file_header_.read(*backend_);

    // Build master_chunk_offsets_ index
    uint64_t current_block_offset = backend_->tell(); // After FileHeader
    while (current_block_offset != 0) {
        backend_->seek(current_block_offset);
        ChunkOffsetsBlock block;
        block.read(*backend_);

        // Verify the integrity of the index block itself
        Blake3StreamHasher block_hasher;
        block_hasher.update(std::span<const uint64_t>(block.offsets_and_pointer));
        if (block_hasher.finalize_128() != block.hash) {
            throw std::runtime_error("ChunkOffsetsBlock integrity check failed. The file index may be corrupt.");
        }
        
        // Append valid chunk offsets from this block
        for (size_t i = 0; i < block.capacity(); ++i) {
            if (block.offsets_and_pointer[i] != 0) { // Only add non-zero offsets
                master_chunk_offsets_.push_back(block.offsets_and_pointer[i]);
            } else {
                // If we hit a zero, it means this block is not fully filled yet,
                // and subsequent entries in this block (and future blocks) are also empty.
                // This is important for files that are still being appended to.
                break; 
            }
        }
        current_block_offset = block.get_next_index_offset();
    }

    if (master_chunk_offsets_.empty()) {
        // This might be a valid state for a newly created, empty file.
        // Or an error if we expect chunks. For now, just log/warn.
        // throw std::runtime_error("No chunks found in file: " + filepath);
    }
}

Chunk DataReader::get_chunk(const size_t index) {
    if (index >= master_chunk_offsets_.size()) {
        throw std::out_of_range("Chunk index out of range.");
    }

    uint64_t chunk_offset = master_chunk_offsets_[index];
    backend_->seek(chunk_offset);

    Chunk chunk;
    chunk.read(*backend_);

    // Sanity check on the number of dimensions read from the file.
    if (chunk.shape.size() > MAX_SHAPE_DIMENSIONS) {
        throw std::runtime_error("Chunk shape has an excessive number of dimensions (> " + std::to_string(MAX_SHAPE_DIMENSIONS) + "). File may be corrupt.");
    }

    // Decompress data if necessary
    if (chunk.type == ChunkDataType::ZSTD_COMPRESSED) {
        // We need the original uncompressed size to decompress.
        // For now, we'll assume the original size is stored somewhere or can be inferred.
        // A robust solution would store original_size in the Chunk header or metadata.
        // For MVP, let's assume the raw data size can be calculated from shape and dtype.
        
        // Calculate expected raw data size
        size_t num_elements = 1;
        for (const uint32_t dim : chunk.shape) {
            if (dim == 0) break; // Null terminator
            num_elements *= dim;
        }

        const size_t element_size = get_dtype_size(chunk.dtype);
        if (element_size == 0) {
            throw std::runtime_error("Unsupported DType for decompression size calculation.");
        }
        const size_t expected_raw_size = num_elements * element_size;

        chunk.data = decompress_zstd(chunk.data, expected_raw_size);
        chunk.type = ChunkDataType::RAW; // Mark as raw after decompression
    }
    // TODO: Implement other decompression/transformation types

    if (const blake3_hash128_t actual_hash = calculate_blake3_hash128<const uint8_t>(chunk.data);
        actual_hash != chunk.hash) {
        throw std::runtime_error("Chunk data integrity check failed for chunk index " + std::to_string(index));
    }

    return chunk;
}

std::vector<std::vector<uint8_t>> DataReader::get_chunk_slice(size_t start_index, size_t end_index) {
    if (start_index >= master_chunk_offsets_.size() || start_index > end_index) {
        throw std::out_of_range("Invalid slice indices.");
    }
    if (end_index >= master_chunk_offsets_.size()) {
        end_index = master_chunk_offsets_.size() - 1;
    }

    std::vector<std::vector<uint8_t>> slice_data;
    for (size_t i = start_index; i <= end_index; ++i) {
        Chunk chunk = get_chunk(i); // Reuses get_chunk logic
        slice_data.push_back(std::move(chunk.data));
    }
    return slice_data;
}

} // namespace cryptodd
