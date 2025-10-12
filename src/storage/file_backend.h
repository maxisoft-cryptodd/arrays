#pragma once

#include "i_storage_backend.h"
#include <filesystem>
#include <fstream>

namespace cryptodd::storage {

// File-based storage backend
class FileBackend final : public IStorageBackend {
private:
    using off_type = std::fstream::off_type;

    std::fstream file_;
    std::filesystem::path filepath_;

public:
    explicit FileBackend(std::filesystem::path filepath,
                         std::ios_base::openmode mode = std::ios_base::in | std::ios_base::out | std::ios_base::binary);

    ~FileBackend() override;

    std::expected<size_t, std::string> read(std::span<std::byte> buffer) override;
    std::expected<size_t, std::string> write(std::span<const std::byte> data) override;
    std::expected<void, std::string> seek(uint64_t offset) override;
    std::expected<uint64_t, std::string> tell() override;
    std::expected<void, std::string> flush() override;
    std::expected<void, std::string> rewind() override;
    [[nodiscard]] std::expected<uint64_t, std::string> size() override;
};

} // namespace cryptodd::storage
