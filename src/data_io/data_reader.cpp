#include "data_reader.h"

#include "blake3_stream_hasher.h"
#include "../storage/file_backend.h"
#include "../storage/memory_backend.h"

#include <filesystem> // For std::filesystem::exists
#include <format>
#include <span>

namespace cryptodd {

ZstdCompressor& DataReader::get_zstd_compressor() const {
    // Thread-safe lazy initialization
    std::call_once(zstd_init_flag_, [this]() {
        // For decompression, we don't need a dictionary or a specific level.
        zstd_compressor_.emplace();
    });
    return *zstd_compressor_;
}

std::expected<std::unique_ptr<DataReader>, std::string> DataReader::open(const std::filesystem::path& filepath) {
    if (!std::filesystem::exists(filepath)) {
        return std::unexpected("File does not exist: " + filepath.string());
    }
    try {
        auto backend = std::make_unique<storage::FileBackend>(filepath, std::ios_base::in | std::ios_base::binary);
        return std::make_unique<DataReader>(Create{}, std::move(backend));
    } catch (const std::exception& e) {
        return std::unexpected(std::format("Failed to open file '{}': {}", filepath.string(), e.what()));
    }
}

std::expected<std::unique_ptr<DataReader>, std::string> DataReader::open_in_memory(std::unique_ptr<IStorageBackend> backend) {
    if (!backend) {
        return std::unexpected("Provided backend is null.");
    }
    try {
        return std::make_unique<DataReader>(Create{}, std::move(backend));
    } catch (const std::exception& e) {
        return std::unexpected(std::format("Failed to open in-memory reader: {}", e.what()));
    }
}
std::expected<std::unique_ptr<DataReader>, std::string> DataReader::open_in_memory()
{
    return open_in_memory(std::move(std::make_unique<storage::MemoryBackend>()));
}

DataReader::DataReader(Create, std::unique_ptr<IStorageBackend>&& backend) : backend_(std::move(backend)) {
    // The constructor must handle potential I/O errors from the backend.
    // Since constructors can't return std::expected, we throw an exception
    // which the static `open` factory functions will catch and convert to an error.
    auto read_header = [this] -> std::expected<void, std::string> {
        if (auto res = file_header_.read(*backend_); !res) {
            return std::unexpected(res.error());
        }

        // Build master_chunk_offsets_ index
        auto tell_res = backend_->tell();
        if (!tell_res) return std::unexpected(tell_res.error());

        uint64_t current_block_offset = *tell_res;
        while (current_block_offset != 0) {
            if (auto seek_res = backend_->seek(current_block_offset); !seek_res) {
                return std::unexpected(seek_res.error());
            }
            ChunkOffsetsBlock block;
            if (auto read_res = block.read(*backend_); !read_res) {
                return std::unexpected(read_res.error());
            }

            // Verify the integrity of the index block itself
            Blake3StreamHasher block_hasher;
            block_hasher.update(std::span(block.offsets_and_pointer()));
            if (block_hasher.finalize_128() != block.hash()) {
                return std::unexpected("ChunkOffsetsBlock integrity check failed. The file index may be corrupt.");
            }

            // Append valid chunk offsets from this block
            for (size_t i = 0; i < block.capacity(); ++i) {
                if (block.offsets_and_pointer()[i] != 0) { // Only add non-zero offsets
                    master_chunk_offsets_.push_back(block.offsets_and_pointer()[i]);
                } else {
                    // If we hit a zero, it means this block is not fully filled yet,
                    // and subsequent entries in this block (and future blocks) are also empty.
                    // This is important for files that are still being appended to.
                    current_block_offset = 0; // End the loop
                    break;
                }
            }
            if (current_block_offset != 0) { // If the loop wasn't broken by a zero offset
                current_block_offset = block.get_next_index_offset();
            }
        }
        return {};
    }();

    if (!read_header) {
        throw std::runtime_error(read_header.error());
    }
}

std::expected<Chunk, std::string> DataReader::get_chunk(const size_t index) {
    if (index >= master_chunk_offsets_.size()) {
        return std::unexpected(std::format("Chunk index {} is out of range (total chunks: {}).", index, master_chunk_offsets_.size()));
    }

    uint64_t chunk_offset = master_chunk_offsets_[index];
    if (auto seek_res = backend_->seek(chunk_offset); !seek_res) {
        return std::unexpected(seek_res.error());
    }

    Chunk chunk;
    if (auto read_res = chunk.read(*backend_); !read_res) {
        return std::unexpected(read_res.error());
    }

    if (chunk.shape().size() > MAX_SHAPE_DIMENSIONS) {
        return std::unexpected(std::format("Chunk {} shape has an excessive number of dimensions (> {}). File may be corrupt.", index, MAX_SHAPE_DIMENSIONS));
    }

    // Decompress data if necessary
    if (chunk.type() == ChunkDataType::ZSTD_COMPRESSED) {
        auto& compressor = get_zstd_compressor();
        auto decompressed_result = compressor.decompress(chunk.data());

        if (!decompressed_result) {
            return std::unexpected(std::format("Failed to decompress chunk {}: {}", index, decompressed_result.error()));
        }

        // --- REGRESSION FIX: Validate decompressed size ---
        size_t num_elements = 1;
        for (const uint32_t dim : chunk.get_shape()) {
            num_elements *= dim;
        }
        const size_t element_size = get_dtype_size(chunk.dtype());
        if (element_size == 0) {
            return std::unexpected(std::format("Unsupported DType ({}) for chunk {}.", static_cast<int>(chunk.dtype()), index));
        }
        const size_t expected_raw_size = num_elements * element_size;

        if (decompressed_result->size() != expected_raw_size) {
            return std::unexpected(std::format("Decompressed size mismatch for chunk {}. Expected {}, got {}.", index, expected_raw_size, decompressed_result->size()));
        }

        chunk.set_data(std::move(*decompressed_result));
        chunk.set_type(ChunkDataType::RAW);
    }
    // TODO: Implement other decompression/transformation types

    const blake3_hash128_t actual_hash = calculate_blake3_hash128(std::span<const std::byte>(chunk.data()));
    if (actual_hash != chunk.hash()) {
        return std::unexpected(std::format("Chunk data integrity check failed for chunk index {}.", index));
    }

    return chunk;
}

std::expected<memory::vector<memory::vector<std::byte>>, std::string> DataReader::get_chunk_slice(size_t start_index, size_t end_index) {
    if (start_index >= master_chunk_offsets_.size() || start_index > end_index) {
        return std::unexpected("Invalid slice indices for get_chunk_slice.");
    }
    if (end_index >= master_chunk_offsets_.size()) {
        end_index = master_chunk_offsets_.size() - 1;
    }

    memory::vector<memory::vector<std::byte>> slice_data;
    for (size_t i = start_index; i <= end_index; ++i) {
        auto chunk_result = get_chunk(i);
        if (!chunk_result) {
            return std::unexpected(std::format("Failed to retrieve chunk {} for slice: {}", i, chunk_result.error()));
        }
        slice_data.push_back(std::move(chunk_result->data()));
    }
    return slice_data;
}

} // namespace cryptodd
