#pragma once

#include "i_storage_backend.h"

namespace cryptodd::storage {

// In-memory storage backend
class MemoryBackend final : public IStorageBackend {
private:
    std::vector<std::byte> buffer_;
    uint64_t current_pos_ = 0;

public:
    explicit MemoryBackend(size_t initial_capacity = 0);

    std::expected<size_t, std::string> read(std::span<std::byte> buffer) override;
    std::expected<size_t, std::string> write(std::span<const std::byte> data) override;
    std::expected<void, std::string> seek(uint64_t offset) override;
    std::expected<uint64_t, std::string> tell() override;
    std::expected<void, std::string> flush() override;
    std::expected<void, std::string> rewind() override;
    [[nodiscard]] std::expected<uint64_t, std::string> size() const override;
};

} // namespace cryptodd::storage