#include "zstd_compressor.h"
#include "zstd.h" // zstd.h is ONLY included here!

#include <memory> // For std::unique_ptr
#include <stdexcept>
#include <string>

namespace cryptodd {

// --- Custom Deleters for Zstd Resources ---
// These deleters teach std::unique_ptr how to free the Zstd handles.

struct ZstdCctxDeleter {
    void operator()(ZSTD_CCtx* ptr) const { ZSTD_freeCCtx(ptr); }
};
struct ZstdDctxDeleter {
    void operator()(ZSTD_DCtx* ptr) const { ZSTD_freeDCtx(ptr); }
};
struct ZstdCdictDeleter {
    void operator()(ZSTD_CDict* ptr) const { ZSTD_freeCDict(ptr); }
};
struct ZstdDdictDeleter {
    void operator()(ZSTD_DDict* ptr) const { ZSTD_freeDDict(ptr); }
};

// --- Type Aliases for Readability ---
using CtxPtr = std::unique_ptr<ZSTD_CCtx, ZstdCctxDeleter>;
using DctxPtr = std::unique_ptr<ZSTD_DCtx, ZstdDctxDeleter>;
using CdictPtr = std::unique_ptr<ZSTD_CDict, ZstdCdictDeleter>;
using DdictPtr = std::unique_ptr<ZSTD_DDict, ZstdDdictDeleter>;


// The full definition of our implementation struct.
// It now uses smart pointers for automatic resource management.
struct ZstdCompressor::Impl {
    CtxPtr cctx;
    DctxPtr dctx;
    CdictPtr cdict;
    DdictPtr ddict;
    int compression_level = DEFAULT_COMPRESSION_LEVEL;

    // Constructor to initialize all resources.
    // Notice the lack of a destructor! RAII takes care of it.
    explicit Impl(std::span<const uint8_t> dict, int level)
        // Initialize smart pointers directly from the ZSTD create functions.
        : cctx(ZSTD_createCCtx()),
          dctx(ZSTD_createDCtx()),
          compression_level(level)
    {
        if (!cctx || !dctx) {
            throw std::runtime_error("Failed to create ZSTD contexts.");
        }

        if (!dict.empty()) {
            // .reset() assigns a new raw pointer for the unique_ptr to manage.
            cdict.reset(ZSTD_createCDict(dict.data(), dict.size(), compression_level));
            ddict.reset(ZSTD_createDDict(dict.data(), dict.size()));
            if (!cdict || !ddict) {
                throw std::runtime_error("Failed to create ZSTD dictionary contexts.");
            }
        }
    }
};

// --- Public Class Implementation ---

ZstdCompressor::ZstdCompressor(std::span<const uint8_t> dict, const int level)
    : pimpl_(std::make_unique<Impl>(dict, level))
{
    // Validate level here after Impl is created to ensure consistency.
    if (level > ZSTD_maxCLevel() || level < ZSTD_minCLevel())
    {
        throw std::runtime_error("Invalid zstd compression level.");
    }
}
ZstdCompressor::ZstdCompressor(const int level): ZstdCompressor({}, level)
{
}

// The destructor and move members are still required here as before,
// to ensure Impl is a complete type when they are defined.
ZstdCompressor::~ZstdCompressor() = default;
ZstdCompressor::ZstdCompressor(ZstdCompressor&&) noexcept = default;
ZstdCompressor& ZstdCompressor::operator=(ZstdCompressor&&) noexcept = default;

std::vector<uint8_t> ZstdCompressor::compress(std::span<const uint8_t> uncompressed_data) {
    const size_t compressed_bound = ZSTD_compressBound(uncompressed_data.size());
    std::vector<uint8_t> compressed_data(compressed_bound);
    size_t compressed_size = 0;

    if (pimpl_->cdict) {
        compressed_size = ZSTD_compress_usingCDict(pimpl_->cctx.get(), compressed_data.data(), compressed_data.size(),
                                                   uncompressed_data.data(), uncompressed_data.size(), pimpl_->cdict.get());
    } else {
        compressed_size = ZSTD_compressCCtx(pimpl_->cctx.get(), compressed_data.data(), compressed_data.size(),
                                            uncompressed_data.data(), uncompressed_data.size(), pimpl_->compression_level);
    }

    if (ZSTD_isError(compressed_size)) {
        throw std::runtime_error(std::string("ZSTD compression failed: ") + ZSTD_getErrorName(compressed_size));
    }
    compressed_data.resize(compressed_size);
    return compressed_data;
}

std::vector<uint8_t> ZstdCompressor::decompress(std::span<const uint8_t> compressed_data) {
    const unsigned long long decompressed_size = ZSTD_getFrameContentSize(compressed_data.data(), compressed_data.size());

    if (decompressed_size == ZSTD_CONTENTSIZE_ERROR || decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        throw std::runtime_error("Invalid ZSTD frame or unknown content size.");
    }

    std::vector<uint8_t> decompressed_data(decompressed_size);
    size_t result_size = 0;

    if (pimpl_->ddict) {
        result_size = ZSTD_decompress_usingDDict(pimpl_->dctx.get(), decompressed_data.data(), decompressed_data.size(),
                                                 compressed_data.data(), compressed_data.size(), pimpl_->ddict.get());
    } else {
        result_size = ZSTD_decompressDCtx(pimpl_->dctx.get(), decompressed_data.data(), decompressed_data.size(),
                                          compressed_data.data(), compressed_data.size());
    }

    if (ZSTD_isError(result_size) || result_size != decompressed_size) {
        throw std::runtime_error("ZSTD decompression failed or produced unexpected size.");
    }

    return decompressed_data;
}

// ReSharper disable once CppMemberFunctionMayBeConst
void ZstdCompressor::set_level(const int level)
{
    if (level > ZSTD_maxCLevel() || level < ZSTD_minCLevel())
    {
        throw std::runtime_error("Invalid zstd compression level.");
    }
    // Cannot change dictionary compression level after creation,
    // so we only update the level for non-dictionary compression.
    this->pimpl_->compression_level = level;
}

} // namespace cryptodd