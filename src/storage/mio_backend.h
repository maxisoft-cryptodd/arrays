#pragma once

#include "i_storage_backend.h"

#include <filesystem>
#include <memory>

namespace cryptodd::storage {

class MioBackend final : public IStorageBackend {
private:
    struct Impl; // Forward-declaration for PIMPL
    std::unique_ptr<Impl> pimpl_;
    std::filesystem::path filepath_;
    uint64_t current_pos_ = 0;
    uint64_t logical_size_ = 0; // Tracks the logical size, independent of physical allocation.
    bool writable_ = false;
    
    std::expected<void, std::string> remap(uint64_t required_size); // Kept private

public:
    explicit MioBackend(std::filesystem::path filepath,
                        std::ios_base::openmode mode = std::ios_base::in | std::ios_base::out | std::ios_base::binary);

    ~MioBackend() override;

    // MioBackend is move-only because mio mappings are move-only.
    MioBackend(const MioBackend&) = delete;
    MioBackend& operator=(const MioBackend&) = delete;
    MioBackend(MioBackend&&) noexcept = default;
    MioBackend& operator=(MioBackend&&) noexcept = default;

    std::expected<size_t, std::string> read(std::span<std::byte> buffer) override;
    std::expected<size_t, std::string> write(std::span<const std::byte> data) override;
    std::expected<void, std::string> seek(uint64_t offset) override;
    std::expected<uint64_t, std::string> tell() override;
    std::expected<void, std::string> flush() override;
    std::expected<void, std::string> rewind() override;
    [[nodiscard]] std::expected<uint64_t, std::string> size() override;
};

} // namespace cryptodd::storage
