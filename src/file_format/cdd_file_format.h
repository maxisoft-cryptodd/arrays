#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <array>
#include <expected>
#include <span>

#include "../storage/i_storage_backend.h"
#include "../memory/allocator.h"
#include "blake3_stream_hasher.h"

namespace cryptodd {

// --- Constants and Enums ---

constexpr uint32_t CDD_MAGIC = 0xCDDBEEF;
constexpr uint16_t CDD_VERSION = 1;
constexpr size_t MAX_SHAPE_DIMENSIONS = 32;

enum class ChunkOffsetType : uint16_t {
    RAW = 1,
    LZ4_COMPRESSED = 2
};

enum class ChunkDataType : uint16_t {
    RAW = 0,
    ZSTD_COMPRESSED = 1,

    // Orderbook Codecs (F16 internal precision, returned as F32)
    OKX_OB_SIMD_F16_AS_F32 = 2,
    OKX_OB_SIMD_F32 = 3,
    BINANCE_OB_SIMD_F16_AS_F32 = 4,
    BINANCE_OB_SIMD_F32 = 5,
    GENERIC_OB_SIMD_F16_AS_F32 = 6,
    GENERIC_OB_SIMD_F32 = 7,

    // Temporal 1D Codecs
    TEMPORAL_1D_SIMD_F16_XOR_SHUFFLE_AS_F32 = 8,
    TEMPORAL_1D_SIMD_F32_XOR_SHUFFLE = 9,
    TEMPORAL_1D_SIMD_I64_XOR = 10,
    TEMPORAL_1D_SIMD_I64_DELTA = 11,

    // Temporal 2D Codecs
    TEMPORAL_2D_SIMD_F16_AS_F32 = 12,
    TEMPORAL_2D_SIMD_F32 = 13,
    TEMPORAL_2D_SIMD_I64 = 14
};

enum class DType : uint16_t {
    FLOAT16 = 0,
    FLOAT32 = 1,
    FLOAT64 = 2,
    INT8 = 3,
    UINT8 = 4,
    INT16 = 5,
    UINT16 = 6,
    INT32 = 7,
    UINT32 = 8,
    INT64 = 9,
    UINT64 = 10,
    BFLOAT16 = 11
};

enum ChunkFlags : uint64_t {
    NONE = 0,
    LZ4 = 1 << 0,
    ZSTD = 1 << 1,
    LITTLE_ENDIAN = 1 << 2,
    BIG_ENDIAN = 1 << 3,
    DOWN_CAST_8 = 1 << 4,
    DOWN_CAST_16 = 1 << 5,
    DOWN_CAST_32 = 1 << 6,
    DOWN_CAST_64 = 1 << 7,
    DOWN_CAST_128 = 1 << 8,
    RESERVED = 1ULL << 63
};

/**
 * @brief Gets the size of a DType in bytes.
 * @param dtype The data type.
 * @return The size in bytes.
 */
constexpr size_t get_dtype_size(const DType dtype) {
    switch (dtype) {
        case DType::FLOAT16: case DType::BFLOAT16: return 2;
        case DType::FLOAT32: return 4;
        case DType::FLOAT64: return 8;
        case DType::INT8: case DType::UINT8: return 1;
        case DType::INT16: case DType::UINT16: return 2;
        case DType::INT32: case DType::UINT32: return 4;
        case DType::INT64: case DType::UINT64: return 8;
    }
    return 0; // Should be unreachable if all DTypes are handled
}

// --- File Format Structures ---

struct InternalMetadata {
    uint64_t chunk_offsets_block_capacity;

    // Read/Write would be implemented if this struct becomes more complex
};

/**
 * @brief Represents the main header of a .cdd file, containing magic, version, and metadata.
 */
class FileHeader {
    using IStorageBackend = storage::IStorageBackend;
public:
    [[nodiscard]] uint32_t magic() const { return magic_; }
    [[nodiscard]] uint16_t version() const { return version_; }
    [[nodiscard]] const memory::vector<std::byte>& internal_metadata() const { return internal_metadata_; }
    [[nodiscard]] const memory::vector<std::byte>& user_metadata() const { return user_metadata_; }
    
    /** @brief Sets the internal metadata, taking ownership of the provided vector. */
    void set_internal_metadata(memory::vector<std::byte> metadata) { internal_metadata_ = std::move(metadata); }
    /** @brief Sets the user-defined metadata, taking ownership of the provided vector. */
    void set_user_metadata(memory::vector<std::byte> metadata) { user_metadata_ = std::move(metadata); }

    std::expected<void, std::string> write(IStorageBackend& backend) const;
    std::expected<void, std::string> read(IStorageBackend& backend);

private:
    uint32_t magic_ = CDD_MAGIC;
    uint16_t version_ = CDD_VERSION;
    memory::vector<std::byte> internal_metadata_;
    memory::vector<std::byte> user_metadata_;
};

/**
 * @brief Represents a block in the file that stores offsets to data chunks.
 * These blocks are chained together to form a master index.
 */
class ChunkOffsetsBlock {
    using IStorageBackend = storage::IStorageBackend;
public:
    /** @brief The number of chunk offsets this block can hold (excluding the pointer to the next block). */
    [[nodiscard]] size_t capacity() const;
    /** @brief Gets the file offset of the next ChunkOffsetsBlock in the chain. */
    [[nodiscard]] uint64_t get_next_index_offset() const;
    /** @brief Sets the file offset of the next ChunkOffsetsBlock in the chain. */
    void set_next_index_offset(uint64_t offset);

    // Other getters/setters
    [[nodiscard]] uint32_t size() const { return size_; }
    void set_size(uint32_t size) { size_ = size; }

    [[nodiscard]] ChunkOffsetType type() const { return type_; }
    void set_type(ChunkOffsetType type) { type_ = type; }

    [[nodiscard]] const blake3_hash128_t& hash() const { return hash_; }
    void set_hash(const blake3_hash128_t& hash) { hash_ = hash; }

    [[nodiscard]] const memory::vector<uint64_t>& offsets_and_pointer() const { return offsets_and_pointer_; }
    void set_offsets_and_pointer(memory::vector<uint64_t> offsets) { offsets_and_pointer_ = std::move(offsets); }

    std::expected<void, std::string> write(IStorageBackend& backend) const;
    std::expected<void, std::string> read(IStorageBackend& backend);

private:
    uint32_t size_{};
    ChunkOffsetType type_{};
    blake3_hash128_t hash_{};
    memory::vector<uint64_t> offsets_and_pointer_;
};

/**
 * @brief Represents a single, self-contained block of data (a "chunk") with its associated metadata.
 */
class Chunk {
    using IStorageBackend = storage::IStorageBackend;
public:
    /**
     * @brief Returns a view of the chunk's shape, excluding the null terminator used for file format compatibility.
     */
    [[nodiscard]] std::span<const uint32_t> get_shape() const;
    /** @brief Calculates the total number of elements in the chunk based on its shape. */
    [[nodiscard]] size_t num_elements() const;
    /** @brief Calculates the expected size of the raw data in bytes based on dtype and shape. */
    [[nodiscard]] size_t expected_size() const
    {
        return get_dtype_size(dtype_) * num_elements();
    }

    // Getters
    [[nodiscard]] uint32_t size() const { return size_; }
    [[nodiscard]] ChunkDataType type() const { return type_; }
    [[nodiscard]] DType dtype() const { return dtype_; }
    [[nodiscard]] const blake3_hash128_t& hash() const { return hash_; }
    [[nodiscard]] uint64_t flags() const { return flags_; }
    [[nodiscard]] const memory::vector<uint32_t>& shape() const { return shape_; }
    [[nodiscard]] const memory::vector<std::byte>& data() const { return data_; }
    /** @brief Gets a mutable reference to the chunk's data vector. */
    [[nodiscard]] memory::vector<std::byte>& data() { return data_; }

    // Setters
    void set_size(uint32_t size) { size_ = size; }
    void set_type(ChunkDataType type) { type_ = type; }
    void set_dtype(DType dtype) { dtype_ = dtype; }
    void set_hash(const blake3_hash128_t& hash) { hash_ = hash; }
    void set_flags(uint64_t flags) { flags_ = flags; }
    void set_shape(memory::vector<uint32_t> shape) { shape_ = std::move(shape); }
    /** @brief Sets the chunk's data, taking ownership of the provided vector. */
    void set_data(memory::vector<std::byte> data) { data_ = std::move(data); }

    std::expected<void, std::string> write(IStorageBackend& backend) const;
    std::expected<void, std::string> read(IStorageBackend& backend);

private:
    uint32_t size_{};
    ChunkDataType type_{};
    DType dtype_{};
    blake3_hash128_t hash_{};
    uint64_t flags_{};
    memory::vector<uint32_t> shape_;
    memory::vector<std::byte> data_;
};

} // namespace cryptodd
