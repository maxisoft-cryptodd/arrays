#pragma once

#include <vector>
#include <span>
#include <cstdint>
#include <cstring>
#include <expected>
#include <vector>
#include <string>
#include <format>

#include "../memory/allocator.h"
#include "../storage/i_storage_backend.h"

namespace cryptodd::serialization {

// --- Helper Functions for Serialization/Deserialization ---

// Writes a POD type to the storage backend at a specific offset
template<typename T>
inline std::expected<size_t, std::string> write_pod_at(storage::IStorageBackend& backend, const uint64_t offset, const T& value) {
    if (auto result = backend.seek(offset); !result) {
        return std::unexpected(result.error());
    }
    return backend.write({reinterpret_cast<const std::byte*>(&value), sizeof(T)});
}

// Writes a POD type to the storage backend
template<typename T>
inline std::expected<size_t, std::string> write_pod(storage::IStorageBackend& backend, const T& value) {
    return backend.write({reinterpret_cast<const std::byte*>(&value), sizeof(T)});
}

// Reads a POD type from the storage backend
template<typename T>
inline std::expected<T, std::string> read_pod(storage::IStorageBackend& backend) {
    T value{};
    auto buffer = std::as_writable_bytes(std::span(&value, 1));
    auto read_result = backend.read(buffer);
    if (!read_result) {
        return std::unexpected(read_result.error());
    }
    if (*read_result != sizeof(T)) {
        return std::unexpected("Failed to read complete POD object.");
    }
    return value;
}

// Writes a vector of POD types
template<typename T>
inline std::expected<size_t, std::string> write_vector_pod(storage::IStorageBackend& backend, std::span<const T> vec) {
    auto result = write_pod(backend, static_cast<uint32_t>(vec.size()));
    if (!result) {
        return result;
    }
    if (!vec.empty()) {
        auto data_result = backend.write(std::as_bytes(vec));
        if (!data_result) {
            return data_result;
        }
        *result += *data_result;
    }
    return result;
}

// Reads a vector of POD types
template<typename T>
inline std::expected<memory::vector<T>, std::string> read_vector_pod(storage::IStorageBackend& backend) {
    auto size_result = read_pod<uint32_t>(backend);
    if (!size_result) {
        return std::unexpected(size_result.error());
    }
    memory::vector<T> vec(*size_result);
    if (*size_result > 0) {
        auto read_result = backend.read(std::as_writable_bytes(std::span(vec)));
        if (!read_result) {
            return std::unexpected(read_result.error());
        }
        if (*read_result != vec.size() * sizeof(T)) {
            return std::unexpected("Failed to read complete vector data.");
        }
    }
    return vec;
}

// Writes a byte vector (blob) with a length prefix
inline std::expected<size_t, std::string> write_blob(storage::IStorageBackend& backend, std::span<const std::byte> blob) {
    auto result = write_pod(backend, static_cast<uint32_t>(blob.size()));
    if (!result) {
        return result;
    }
    if (!blob.empty()) {
        auto data_result = backend.write(blob);
        if (!data_result) {
            return data_result;
        }
        *result += *data_result;
    }
    return result;
}

// Reads a byte vector (blob) with a length prefix
inline std::expected<memory::vector<std::byte>, std::string> read_blob(storage::IStorageBackend& backend) {
    auto size_result = read_pod<uint32_t>(backend);
    if (!size_result) {
        return std::unexpected(size_result.error());
    }
    memory::vector<std::byte> blob(*size_result);
    if (*size_result > 0) {
        auto read_result = backend.read(blob);
        if (!read_result) {
            return std::unexpected(read_result.error());
        }
        if (*read_result != blob.size()) {
            return std::unexpected("Failed to read complete blob data.");
        }
    }
    return blob;
}

// Serializes a vector of PODs into a byte buffer, prefixed with its count.
template<typename T>
inline memory::vector<std::byte> serialize_vector_pod_to_buffer(std::span<const T> vec) {
    const auto count = static_cast<uint32_t>(vec.size());
    const size_t data_size_bytes = vec.size_bytes();
    const size_t total_size = sizeof(count) + data_size_bytes;

    memory::vector<std::byte> buffer(total_size);
    
    // Write count
    std::memcpy(buffer.data(), &count, sizeof(count));

    // Write data
    if (data_size_bytes > 0) {
        std::memcpy(buffer.data() + sizeof(count), vec.data(), data_size_bytes);
    }

    return buffer;
}

// Deserializes a byte buffer (prefixed with a count) into a vector of PODs.
template<typename T>
inline std::expected<memory::vector<T>, std::string> deserialize_vector_pod_from_buffer(std::span<const std::byte> buffer) {
    if (buffer.size() < sizeof(uint32_t)) {
        return std::unexpected("Buffer is too small to contain a size prefix.");
    }

    uint32_t count;
    std::memcpy(&count, buffer.data(), sizeof(count));

    const size_t expected_data_size = static_cast<size_t>(count) * sizeof(T);
    const size_t expected_total_size = sizeof(uint32_t) + expected_data_size;

    if (buffer.size() != expected_total_size) {
        return std::unexpected(std::format("Buffer size mismatch. Expected {} bytes, got {} bytes.", expected_total_size, buffer.size()));
    }

    memory::vector<T> vec(count);
    if (count > 0) {
        std::memcpy(vec.data(), buffer.data() + sizeof(uint32_t), expected_data_size);
    }

    return vec;
}

} // namespace cryptodd::serialization
