#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace cryptodd::storage {

// Interface for storage backends
class IStorageBackend {
public:
    virtual ~IStorageBackend() = default;

    /** @return The number of bytes read on success. */
    virtual std::expected<size_t, std::string> read(std::span<std::byte> buffer) = 0;

    /** @return The number of bytes written on success. */
    virtual std::expected<size_t, std::string> write(std::span<const std::byte> data) = 0;

    /** @return Void on success. */
    virtual std::expected<void, std::string> seek(uint64_t offset) = 0;

    /** @return The current position on success. */
    virtual std::expected<uint64_t, std::string> tell() = 0;

    /** @return Void on success. */
    virtual std::expected<void, std::string> flush() = 0;

    /** @return Void on success. */
    virtual std::expected<void, std::string> rewind() = 0;

    /** @return The total size of the storage on success. */
    [[nodiscard]] virtual std::expected<uint64_t, std::string> size() const = 0;
};

} // namespace cryptodd::storage