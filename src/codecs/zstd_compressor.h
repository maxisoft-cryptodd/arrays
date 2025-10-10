#pragma once

#include "i_compressor.h"
#include <memory>
#include <span>

namespace cryptodd {

class ZstdCompressor final : public ICompressor {
public:
    static constexpr int DEFAULT_COMPRESSION_LEVEL = 1;
    // Constructor takes an optional dictionary.
    explicit ZstdCompressor(std::span<const uint8_t> dict, int level = DEFAULT_COMPRESSION_LEVEL);
    explicit ZstdCompressor(int level = DEFAULT_COMPRESSION_LEVEL);
    ~ZstdCompressor() override;

    // --- Rule of Five for PIMPL ---
    ZstdCompressor(const ZstdCompressor&) = delete;
    ZstdCompressor& operator=(const ZstdCompressor&) = delete;
    ZstdCompressor(ZstdCompressor&&) noexcept;
    ZstdCompressor& operator=(ZstdCompressor&&) noexcept;

    // --- Interface Implementation ---
    std::vector<uint8_t> compress(std::span<const uint8_t> uncompressed_data) override;
    std::vector<uint8_t> decompress(std::span<const uint8_t> compressed_data) override;

    void set_level(int level);

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace cryptodd