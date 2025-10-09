#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <stdexcept>
#include <numeric> // For std::iota
#include <filesystem>
#include <span>

#include "../storage/storage_backend.h"
#include "../file_format/cdd_file_format.h"

namespace cryptodd {

class DataReader {
private:
    std::unique_ptr<IStorageBackend> backend_;
    FileHeader file_header_;
    std::vector<uint64_t> master_chunk_offsets_; // Consolidated index of all chunk offsets

    // Private constructor that takes ownership of a backend
    explicit DataReader(std::unique_ptr<IStorageBackend> backend);

public:
    // Factory function for opening a file for reading
    static DataReader open(const std::filesystem::path& filepath);

    // Factory function for opening an in-memory backend for reading
    // Note: This takes ownership of the backend.
    static DataReader open_in_memory(std::unique_ptr<IStorageBackend> backend);

    // Helper to decompress data using Zstd
    static std::vector<uint8_t> decompress_zstd(std::span<const uint8_t> input, size_t decompressed_size);

    // Returns the FileHeader
    [[nodiscard]] const FileHeader& get_file_header() const { return file_header_; }

    // Returns the total number of chunks in the file
    [[nodiscard]] size_t num_chunks() const { return master_chunk_offsets_.size(); }

    // Retrieves a specific chunk by its index
    Chunk get_chunk(size_t index);

    // Retrieves a slice of chunks, returning a vector of raw data buffers
    std::vector<std::vector<uint8_t>> get_chunk_slice(size_t start_index, size_t end_index);
};

} // namespace cryptodd
