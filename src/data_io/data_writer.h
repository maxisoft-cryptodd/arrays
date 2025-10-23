#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "../codecs/zstd_compressor.h"
#include "../file_format/cdd_file_format.h"
#include "../storage/i_storage_backend.h"

namespace cryptodd {

class DataWriter {
    using IStorageBackend = storage::IStorageBackend;

private:
    std::unique_ptr<IStorageBackend> backend_;
    FileHeader file_header_;
    memory::vector<ChunkOffsetsBlock> chunk_offset_blocks_;
    uint64_t current_chunk_offset_block_start_ = 0;
    size_t current_chunk_offset_block_index_ = 0;
    size_t chunk_offsets_block_capacity_;

    mutable std::optional<ZstdCompressor> zstd_compressor_;
    mutable std::once_flag zstd_init_flag_;

    ZstdCompressor& get_zstd_compressor() const;

    std::expected<void, std::string> write_new_chunk_offsets_block(uint64_t previous_block_offset);

public:
    static constexpr size_t DEFAULT_CHUNK_OFFSETS_BLOCK_CAPACITY = 1024;

    /**
     * @brief Private construction key.
     * This prevents direct construction of DataWriter, forcing use of static factory methods.
     */
    struct Create {
    private:
        Create() = default;
        friend class DataWriter;
    };

    // Constructors are public but can only be called with the 'Create' passkey.
    explicit DataWriter(Create, std::unique_ptr<IStorageBackend>&& backend, size_t chunk_offsets_block_capacity,
                        std::span<const std::byte> user_metadata);

    explicit DataWriter(Create, std::unique_ptr<IStorageBackend>&& backend);

    /**
     * @brief Creates a new file for writing.
     * @param filepath Path to the new file.
     * @param chunk_offsets_block_capacity The number of chunk offsets to store per block.
     * @param user_metadata Optional user-defined metadata to store in the file header.
     * @return A unique_ptr to the DataWriter on success, or an error string.
     */
    static std::expected<std::unique_ptr<DataWriter>, std::string> create_new(const std::filesystem::path& filepath,
                                                                              size_t chunk_offsets_block_capacity = DEFAULT_CHUNK_OFFSETS_BLOCK_CAPACITY,
                                                                              std::span<const std::byte> user_metadata = {});

    /**
     * @brief Opens an existing file for appending.
     * @param filepath Path to the existing file.
     * @return A unique_ptr to the DataWriter on success, or an error string.
     */
    static std::expected<std::unique_ptr<DataWriter>, std::string> open_for_append(const std::filesystem::path& filepath);

    /**
     * @brief Creates a new in-memory writer.
     * @param chunk_offsets_block_capacity The number of chunk offsets to store per block.
     * @param user_metadata Optional user-defined metadata to store in the file header.
     * @return A unique_ptr to the DataWriter on success, or an error string.
     */
    static std::expected<std::unique_ptr<DataWriter>, std::string> create_in_memory(size_t chunk_offsets_block_capacity = DEFAULT_CHUNK_OFFSETS_BLOCK_CAPACITY,
                                                                                    std::span<const std::byte> user_metadata = {});

    /**
     * @brief Appends a new chunk of data.
     * @param type The data type of the chunk.
     * @param dtype The underlying data type of the elements in the chunk.
     * @param flags Flags for the chunk.
     * @param shape The shape of the data.
     * @param source_chunk The chunk containing the data payload to be written. The data will be moved from this chunk.
     * @param raw_data_hash The BLAKE3 hash of the original, unprocessed data.
     * @return The index of the newly appended chunk on success, or an error string.
     */
    std::expected<size_t, std::string> append_chunk(ChunkDataType type, DType dtype, ChunkFlags flags,
                                                  std::span<const int64_t> shape, Chunk& source_chunk,
                                                  blake3_hash256_t raw_data_hash);

    /**
     * @brief Sets the ZSTD compression level for subsequent index block compression.
     * @param level The compression level (1-22).
     */
    void set_compression_level(int level);

    /**
     * @brief Updates the user metadata in the file header.
     * @param user_metadata The new user metadata.
     * @return void on success, or an error string.
     */
    std::expected<void, std::string> set_user_metadata(std::span<const std::byte> user_metadata);

    /**
     * @brief Flushes any buffered data to the underlying storage.
     * @return void on success, or an error string.
     */
    std::expected<void, std::string> flush();

    /**
     * @brief Releases ownership of the underlying storage backend.
     * @return A unique_ptr to the storage backend.
     */
    [[nodiscard]] std::expected<std::unique_ptr<IStorageBackend>, std::string> release_backend();

    /**
     * @brief Returns the total number of chunks written.
     * @return The number of chunks.
     */
    [[nodiscard]] size_t num_chunks() const;
};

} // namespace cryptodd
