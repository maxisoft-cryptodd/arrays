#pragma once

#include <vector>
#include <span>
#include <expected>
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

} // namespace cryptodd::serialization
