#pragma once

#include <vector>
#include <string>
#include <memory>
#include <stdexcept>
#include <expected>
#include <filesystem>
#include <mutex>
#include <span>

#include "../storage/i_storage_backend.h"
#include "../file_format/cdd_file_format.h"
#include "../codecs/zstd_compressor.h"

// Forward declarations to avoid including codec headers in non-codec headers
#include "chunk_offset_codec_allocator_fwd.h"

namespace cryptodd {

    class DataReader {
    using IStorageBackend = storage::IStorageBackend;

    std::unique_ptr<storage::IStorageBackend> backend_;
    FileHeader file_header_;
    memory::vector<uint64_t> master_chunk_offsets_; // Consolidated index of all chunk offsets
    mutable std::unique_ptr<ZstdCompressor> zstd_compressor_;
    mutable std::once_flag zstd_init_flag_;

    uint64_t index_block_offset_{0};
    uint64_t index_block_size_{0};

    std::shared_ptr<ChunkOffsetCodecAllocator> codec_cache_allocator_;

    ZstdCompressor& get_zstd_compressor() const;

public:
    /**
     * @brief Private construction key.
     * This prevents direct construction of DataReader, forcing use of static factory methods.
     */
    struct Create {
    private:
        Create() = default;
        friend class DataReader;
    };

    explicit DataReader(Create, std::unique_ptr<IStorageBackend>&& backend,
                        std::shared_ptr<ChunkOffsetCodecAllocator> codec_allocator = get_chunk_offset_codec_allocator());
    // Factory function for opening a file for reading. Returns an error on failure.
    static std::expected<std::unique_ptr<DataReader>, std::string> open(const std::filesystem::path& filepath);

    // Factory function for opening an in-memory backend for reading. Returns an error on failure.
    static std::expected<std::unique_ptr<DataReader>, std::string> open_in_memory(std::unique_ptr<storage::IStorageBackend> backend);

    // Factory function for opening an in-memory backend for reading. Returns an error on failure.
    static std::expected<std::unique_ptr<DataReader>, std::string> open_in_memory();

    // Returns the FileHeader
    [[nodiscard]] const FileHeader& get_file_header() const { return file_header_; }

    // Returns the total number of chunks in the file
    [[nodiscard]] size_t num_chunks() const { return master_chunk_offsets_.size(); }

    [[nodiscard]] uint64_t get_index_block_offset() const { return index_block_offset_; }
    [[nodiscard]] uint64_t get_index_block_size() const { return index_block_size_; }

    // Retrieves a specific chunk by its index. Returns an error on failure.
    std::expected<Chunk, std::string> get_chunk(size_t index);

    // Retrieves a slice of chunks, returning a vector of raw data buffers. Returns an error on failure.
    std::expected<memory::vector<memory::vector<std::byte>>, std::string> get_chunk_slice(size_t start_index, size_t end_index);

    void set_codec_cache_allocator(decltype(codec_cache_allocator_) codec_allocator);
};

} // namespace cryptodd
