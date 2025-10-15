#include "zstd_compressor.h"
#include "zstd.h" // zstd.h is ONLY included here!

#include <format>
#include <expected>
#include <memory>
#include <stdexcept>
#include <utility>

namespace cryptodd
{

// --- Custom Deleters for Zstd Resources ---
struct ZstdCctxDeleter { void operator()(ZSTD_CCtx* ptr) const { ZSTD_freeCCtx(ptr); } };
struct ZstdDctxDeleter { void operator()(ZSTD_DCtx* ptr) const { ZSTD_freeDCtx(ptr); } };
struct ZstdCdictDeleter { void operator()(ZSTD_CDict* ptr) const { ZSTD_freeCDict(ptr); } };
struct ZstdDdictDeleter { void operator()(ZSTD_DDict* ptr) const { ZSTD_freeDDict(ptr); } };

// --- Type Aliases for Readability ---
using CtxPtr = std::unique_ptr<ZSTD_CCtx, ZstdCctxDeleter>;
using DctxPtr = std::unique_ptr<ZSTD_DCtx, ZstdDctxDeleter>;
using CdictPtr = std::unique_ptr<ZSTD_CDict, ZstdCdictDeleter>;
using DdictPtr = std::unique_ptr<ZSTD_DDict, ZstdDdictDeleter>;

struct ZstdCompressor::Impl
{
    CtxPtr cctx;
    DctxPtr dctx;
    CdictPtr cdict;
    DdictPtr ddict;
    int compression_level = DEFAULT_COMPRESSION_LEVEL;

    explicit Impl(std::span<const std::byte> dict, const int level)
        : cctx(ZSTD_createCCtx()), dctx(ZSTD_createDCtx()), compression_level(level)
    {
        if (!cctx || !dctx)
        {
            throw std::runtime_error("Failed to create ZSTD contexts.");
        }

        if (!dict.empty())
        {
            cdict.reset(ZSTD_createCDict(dict.data(), dict.size(), compression_level));
            ddict.reset(ZSTD_createDDict(dict.data(), dict.size()));
            if (!cdict || !ddict)
            {
                throw std::runtime_error("Failed to create ZSTD dictionary contexts.");
            }
        }
    }
};

ZstdCompressor::ZstdCompressor(std::span<const std::byte> dict, const int level)
{
    if (level > ZSTD_maxCLevel() || level < ZSTD_minCLevel())
    {
        throw std::invalid_argument("Invalid zstd compression level.");
    }
    pimpl_ = std::make_unique<Impl>(std::as_bytes(dict), level);
}

ZstdCompressor::ZstdCompressor(const int level) : ZstdCompressor({}, level) {}

ZstdCompressor::~ZstdCompressor() = default;
ZstdCompressor::ZstdCompressor(ZstdCompressor&&) noexcept = default;
ZstdCompressor& ZstdCompressor::operator=(ZstdCompressor&&) noexcept = default;

size_t ZstdCompressor::do_get_compress_bound(const std::span<const std::byte> uncompressed_data) {
    return ZSTD_compressBound(uncompressed_data.size_bytes());
}

std::expected<size_t, std::string> ZstdCompressor::do_get_decompress_size(std::span<const std::byte> compressed_data) {
    const unsigned long long decompressed_size = ZSTD_getFrameContentSize(compressed_data.data(), compressed_data.size());

    if (decompressed_size == ZSTD_CONTENTSIZE_ERROR) {
        return std::unexpected("Invalid ZSTD frame: content size error.");
    }
    if (decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        return std::unexpected("Cannot decompress ZSTD frame with unknown content size.");
    }
    return static_cast<size_t>(decompressed_size);
}

std::expected<size_t, std::string> ZstdCompressor::do_compress_into(std::span<const std::byte> uncompressed, std::span<std::byte> compressed) {
    const size_t compressed_size = pimpl_->cdict
        ? ZSTD_compress_usingCDict(pimpl_->cctx.get(), compressed.data(), compressed.size(),
                                   uncompressed.data(), uncompressed.size(), pimpl_->cdict.get())
        : ZSTD_compressCCtx(pimpl_->cctx.get(), compressed.data(), compressed.size(),
                            uncompressed.data(), uncompressed.size(), pimpl_->compression_level);

    if (ZSTD_isError(compressed_size)) {
        return std::unexpected(std::format("ZSTD compression failed: {}", ZSTD_getErrorName(compressed_size)));
    }
    return compressed_size;
}

std::expected<size_t, std::string> ZstdCompressor::do_decompress_into(std::span<const std::byte> compressed, std::span<std::byte> decompressed) {
    const size_t result_size = pimpl_->ddict
        ? ZSTD_decompress_usingDDict(pimpl_->dctx.get(), decompressed.data(), decompressed.size(),
                                     compressed.data(), compressed.size(), pimpl_->ddict.get())
        : ZSTD_decompressDCtx(pimpl_->dctx.get(), decompressed.data(), decompressed.size(),
                              compressed.data(), compressed.size());

    if (ZSTD_isError(result_size)) {
        return std::unexpected(std::format("ZSTD decompression failed: {}", ZSTD_getErrorName(result_size)));
    }
    return result_size;
}

void ZstdCompressor::set_level(const int level)
{
    if (level > ZSTD_maxCLevel() || level < ZSTD_minCLevel())
    {
        throw std::invalid_argument("Invalid zstd compression level.");
    }
    pimpl_->compression_level = level;
}

} // namespace cryptodd
