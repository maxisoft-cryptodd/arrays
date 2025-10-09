#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <array>
#include <numeric> // For std::iota
#include <algorithm> // For std::min, std::copy
#include <span>

#include "../storage/storage_backend.h"
#include "blake3_stream_hasher.h"
#include <zstd.h>
#include <lz4.h>

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
    OKX_OB_SIMD_F16 = 2,
    OKX_OB_SIMD_F32 = 3,
    GENERIC_OB_SIMD = 4
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
    UINT64 = 10
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
        case DType::FLOAT16: return 2;
        case DType::FLOAT32: return 4;
        case DType::FLOAT64: return 8;
        case DType::INT8: case DType::UINT8: return 1;
        case DType::INT16: case DType::UINT16: return 2;
        case DType::INT32: case DType::UINT32: return 4;
        case DType::INT64: case DType::UINT64: return 8;
    }
    return 0; // Should be unreachable if all DTypes are handled
}

// --- Helper Functions for Serialization/Deserialization ---

// Writes a POD type to the storage backend

// Writes a POD type to the storage backend at a specific offset
template<typename T>
inline void write_pod_at(IStorageBackend& backend, const uint64_t offset, const T& value) {
    backend.seek(offset);
    backend.write(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&value), sizeof(T)));
}

template<typename T>
inline void write_pod(IStorageBackend& backend, const T& value) {
    backend.write(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&value), sizeof(T)));
}

// Reads a POD type from the storage backend
template<typename T>
inline T read_pod(IStorageBackend& backend) {
    T value{};
    if (backend.read(std::span<uint8_t>(reinterpret_cast<uint8_t*>(&value), sizeof(T))) != sizeof(T)) {
        throw std::runtime_error("Failed to read data in read_pod.");
    }
    return value;
}

// Writes a vector of POD types
template<typename T>
inline void write_vector_pod(IStorageBackend& backend, std::span<const T> vec) {
    write_pod(backend, static_cast<uint32_t>(vec.size()));
    if (!vec.empty()) {
        backend.write(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(vec.data()), vec.size_bytes()));
    }
}

// Reads a vector of POD types
template<typename T>
inline std::vector<T> read_vector_pod(IStorageBackend& backend) {
    auto size = read_pod<uint32_t>(backend);
    std::vector<T> vec(size);
    if (size > 0) { // If there are elements to read
        const size_t bytes_to_read = size * sizeof(T);
        std::span<uint8_t> buffer(reinterpret_cast<uint8_t*>(vec.data()), bytes_to_read);
        if (backend.read(buffer) != bytes_to_read) {
            throw std::runtime_error("Failed to read data in read_vector_pod.");
        }
    }
    return vec;
}

// Writes a byte vector (blob) with a length prefix
inline void write_blob(IStorageBackend& backend, std::span<const uint8_t> blob) {
    write_pod(backend, static_cast<uint32_t>(blob.size()));
    if (!blob.empty()) {
        backend.write(blob);
    }
}

// Reads a byte vector (blob) with a length prefix
inline std::vector<uint8_t> read_blob(IStorageBackend& backend) {
    auto size = read_pod<uint32_t>(backend);
    std::vector<uint8_t> blob(size);
    if (size > 0) {
        if (backend.read(blob) != size) {
            throw std::runtime_error("Failed to read data in read_blob.");
        }
    }
    return blob;
}

// --- File Format Structures ---

struct InternalMetadata {
    uint64_t chunk_offsets_block_capacity;

    void write(IStorageBackend& backend) const {
        write_pod(backend, chunk_offsets_block_capacity);
    }

    void read(IStorageBackend& backend) {
        chunk_offsets_block_capacity = read_pod<uint64_t>(backend);
    } 
};

struct FileHeader {
    uint32_t magic = CDD_MAGIC;
    uint16_t version = CDD_VERSION;
    std::vector<uint8_t> internal_metadata; // Zstd compressed, length prefixed
    std::vector<uint8_t> user_metadata;     // Zstd compressed, length prefixed

    void write(IStorageBackend& backend) const {
        write_pod(backend, magic);
        write_pod(backend, version);
        write_blob(backend, internal_metadata);
        write_blob(backend, user_metadata);
    }

    void read(IStorageBackend& backend) {
        magic = read_pod<uint32_t>(backend);
        version = read_pod<uint16_t>(backend);
        internal_metadata = read_blob(backend);
        user_metadata = read_blob(backend);

        if (magic != CDD_MAGIC) {
            throw std::runtime_error("Invalid CDD file magic.");
        }
        if (version != CDD_VERSION) {
            throw std::runtime_error("Unsupported CDD file version.");
        }
    }
};

struct ChunkOffsetsBlock {
    uint32_t size; // Size of this entire block in bytes (including size, type, hash, and offsets_and_pointer)
    ChunkOffsetType type;
    blake3_hash128_t hash; // BLAKE3 hash of offsets_and_pointer
    std::vector<uint64_t> offsets_and_pointer; // Last element is next_index_offset

    // Capacity of the offsets_and_pointer array (excluding the next_index_offset pointer)
    [[nodiscard]] size_t capacity() const {
        if (offsets_and_pointer.empty()) return 0;
        return offsets_and_pointer.size() - 1;
    }

    // Get the next_index_offset
    [[nodiscard]] uint64_t get_next_index_offset() const {
        if (offsets_and_pointer.empty()) return 0;
        return offsets_and_pointer.back();
    }

    // Set the next_index_offset
    void set_next_index_offset(uint64_t offset) {
        if (offsets_and_pointer.empty()) {
            throw std::runtime_error("Cannot set next_index_offset on an empty offsets_and_pointer vector.");
        }
        offsets_and_pointer.back() = offset;
    }

    void write(IStorageBackend& backend) const {
        uint64_t start_pos = backend.tell();
        write_pod(backend, size);
        write_pod(backend, static_cast<uint16_t>(type));
        write_pod(backend, hash);
        // ReSharper disable once CppTemplateArgumentsCanBeDeduced
        write_vector_pod(backend, std::span<const uint64_t>(offsets_and_pointer));
        // Ensure the recorded size is correct
        if (backend.tell() - start_pos != size) {
            throw std::runtime_error("ChunkOffsetsBlock size mismatch during write.");
        }
    }

    void read(IStorageBackend& backend) {
        uint64_t start_pos = backend.tell();
        size = read_pod<uint32_t>(backend);
        type = static_cast<ChunkOffsetType>(read_pod<uint16_t>(backend));
        hash = read_pod<blake3_hash128_t>(backend);
        offsets_and_pointer = read_vector_pod<uint64_t>(backend);

        // Validate hash (optional, can be done after reading all blocks)
        // Validate size
        if (backend.tell() - start_pos != size) {
            throw std::runtime_error("ChunkOffsetsBlock size mismatch during read.");
        }
    }
};

struct Chunk {
    uint32_t size; // Size of this entire chunk block in bytes (including all fields and data)
    ChunkDataType type;
    DType dtype;
    blake3_hash128_t hash; // BLAKE3 hash of the raw (uncompressed) data
    uint64_t flags;
    std::vector<uint32_t> shape; // Null-terminated, so last element is 0
    std::vector<uint8_t> data;

    void write(IStorageBackend& backend) const {
        uint64_t start_pos = backend.tell();
        write_pod(backend, size);
        write_pod(backend, static_cast<uint16_t>(type));
        write_pod(backend, static_cast<uint16_t>(dtype));
        write_pod(backend, hash);
        write_pod(backend, flags);
        // ReSharper disable once CppTemplateArgumentsCanBeDeduced
        write_vector_pod(backend, std::span<const uint32_t>(shape)); // shape includes the null terminator
        // ReSharper disable once CppTemplateArgumentsCanBeDeduced
        write_blob(backend, std::span<const uint8_t>(data)); // data is length-prefixed by write_blob

        // Ensure the recorded size is correct
        if (backend.tell() - start_pos != size) {
            throw std::runtime_error("Chunk size mismatch during write.");
        }
    }

    void read(IStorageBackend& backend) {
        uint64_t start_pos = backend.tell();
        size = read_pod<uint32_t>(backend);
        type = static_cast<ChunkDataType>(read_pod<uint16_t>(backend));
        dtype = static_cast<DType>(read_pod<uint16_t>(backend));
        hash = read_pod<blake3_hash128_t>(backend);
        flags = read_pod<uint64_t>(backend);
        shape = read_vector_pod<uint32_t>(backend);
        data = read_blob(backend);

        // Validate size
        if (backend.tell() - start_pos != size) {
            throw std::runtime_error("Chunk size mismatch during read.");
        }
    }
};

} // namespace cryptodd
