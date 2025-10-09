#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <stdexcept>
#include <span>

#include "../storage/storage_backend.h"
#include "../file_format/cdd_file_format.h"

namespace cryptodd {

// Tag for constructor dispatching
struct for_append_t {};
inline constexpr for_append_t for_append{};

class DataWriter {
private:
    std::unique_ptr<IStorageBackend> backend_;
    FileHeader file_header_;
    std::vector<ChunkOffsetsBlock> chunk_offset_blocks_;
    uint64_t current_chunk_offset_block_start_ = 0; // Offset to the current ChunkOffsetsBlock in the file
    size_t current_chunk_offset_block_index_ = 0; // Index within the current ChunkOffsetsBlock
    Blake3StreamHasher current_block_hasher_; // Stateful hasher for the current block

    // Configuration for ChunkOffsetsBlock capacity
    size_t chunk_offsets_block_capacity_;

    // Helper to write a new ChunkOffsetsBlock
    void write_new_chunk_offsets_block(uint64_t previous_block_offset);

    // Helper to compress data using Zstd
    static std::vector<uint8_t> compress_zstd(std::span<const uint8_t> input);
    // Helper to update the next_index_offset of the previous ChunkOffsetsBlock
    void update_previous_chunk_offsets_block_pointer(uint64_t previous_block_offset, uint64_t new_block_offset);

    // Generic constructor for creating a new store with a given backend
    explicit DataWriter(std::unique_ptr<IStorageBackend> backend, size_t chunk_offsets_block_capacity,
                        std::span<const uint8_t> user_metadata);

    // Generic constructor for appending to an existing store with a given backend
    explicit DataWriter(std::unique_ptr<IStorageBackend> backend);

    // Constructor for creating a new file
    explicit DataWriter(const std::filesystem::path& filepath, size_t chunk_offsets_block_capacity = 1024,
                        std::span<const uint8_t> user_metadata = {});

    // Constructor for appending to an existing file (now delegates to the backend version)
    explicit DataWriter(const std::filesystem::path& filepath, for_append_t);

  public:
    // Factory function for creating a new file.
    static DataWriter create_new(const std::filesystem::path& filepath, size_t chunk_offsets_block_capacity = 1024,
                                 std::span<const uint8_t> user_metadata = {});

    // Factory function for appending to an existing file
    static DataWriter open_for_append(const std::filesystem::path& filepath);

    // Factory function for creating an in-memory writer
    static DataWriter create_in_memory(size_t chunk_offsets_block_capacity = 1024,
                                       std::span<const uint8_t> user_metadata = {});

    // Appends a raw data chunk
    void append_chunk(ChunkDataType type, DType dtype, uint64_t flags,
                      std::span<const uint32_t> shape, std::span<const uint8_t> data);

    // Flushes all pending writes to the storage backend
    void flush();

    // Releases ownership of the underlying storage backend.
    // This is useful for in-memory testing to pass the backend from a writer to a reader.
    // After calling this, the DataWriter is no longer usable.
    [[nodiscard]] std::unique_ptr<IStorageBackend> release_backend();

    // Returns the total number of chunks written so far
    [[nodiscard]] size_t num_chunks() const;
};

} // namespace cryptodd
