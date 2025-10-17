#include "data_compressor.h"

#include "../codecs/orderbook_simd_codec.h"
#include "../codecs/temporal_1d_simd_codec.h"
#include "../codecs/temporal_2d_simd_codec.h"
#include "../codecs/zstd_compressor.h"
#include "../codecs/codec_constants.h" // For codecs::Orderbook::OKX_DEPTH, etc.
#include <bit> // For std::endian

#include <map>
#include <mutex>
#include <format>
#include <stdexcept>
#include <tuple>

namespace cryptodd
{

namespace {
    // Helper to create a chunk and handle potential errors from encoding.
    DataCompressor::ChunkResult create_chunk_from_result(
        std::expected<memory::vector<std::byte>, std::string>&& result,
        ChunkDataType type,
        DType dtype,
        std::span<const int64_t> shape,
        uint64_t flags)
    {
        if (!result) {
            return std::unexpected(CodecError::from_string(result.error(), ErrorCode::EncodingFailure));
        }
        auto chunk = std::make_unique<Chunk>();
        chunk->set_type(type);
        chunk->set_dtype(dtype);
        chunk->set_shape({shape.begin(), shape.end()});
        chunk->set_data(std::move(*result));
        
        if constexpr (std::endian::native == std::endian::little)
        {
            flags |= ChunkFlags::LITTLE_ENDIAN;
        }
        else if constexpr (std::endian::native == std::endian::big)
        {
            flags |= ChunkFlags::BIG_ENDIAN;
        }
        chunk->set_flags(flags);
        return chunk;
    }
}

struct DataCompressor::Impl
{
    // --- Reusable Workspaces ---
    // These are protected by mutexes to allow a single DataCompressor instance to be
    // shared safely between threads. For maximum performance, create one DataCompressor
    // instance per thread to avoid contention on these locks.
    mutable OrderbookSimdCodecWorkspace ob_workspace_;
    mutable std::mutex ob_workspace_mutex_;
    mutable Temporal1dSimdCodecWorkspace temporal_1d_workspace_;
    mutable std::mutex temporal_1d_workspace_mutex_;
    mutable Temporal2dSimdCodecWorkspace temporal_2d_workspace_;
    mutable std::mutex temporal_2d_workspace_mutex_;


    // --- Codec Caching ---
    // Caches are used to avoid the overhead of recreating codec and compressor
    // instances if the same configuration (level) is used repeatedly.
    // The maps are protected by mutexes for thread-safe access.

    // Key: <depth, features, level>
    using ObCodecKey = std::tuple<size_t, size_t, int>;
    mutable std::map<ObCodecKey, DynamicOrderbookSimdCodec> ob_codecs_cache_;
    mutable std::mutex ob_cache_mutex_;

    // Key: <level>
    mutable std::map<int, Temporal1dSimdCodec> t1d_codecs_cache_;
    mutable std::mutex t1d_cache_mutex_;

    // Key: <num_features, level>
    using T2dCodecKey = std::tuple<size_t, int>;
    mutable std::map<T2dCodecKey, DynamicTemporal2dSimdCodec> t2d_codecs_cache_;
    mutable std::mutex t2d_cache_mutex_;

    // Key: <level>
    mutable std::map<int, OkxObSimdCodec> okx_ob_codecs_cache_;
    mutable std::mutex okx_ob_cache_mutex_;

    // Key: <level>
    mutable std::map<int, BinanceObSimdCodec> binance_ob_codecs_cache_;
    mutable std::mutex binance_ob_cache_mutex_;
    
    // --- Cache Accessor Methods ---

    [[nodiscard]] DynamicOrderbookSimdCodec& get_ob_codec(size_t depth, size_t features, int level) const
    {
        const ObCodecKey key = {depth, features, level};
        std::lock_guard lock(ob_cache_mutex_);
        
        auto it = ob_codecs_cache_.find(key);
        if (it == ob_codecs_cache_.end()) {
            it = ob_codecs_cache_.try_emplace(key, depth, features, std::make_unique<ZstdCompressor>(level)).first;
        }
        return it->second;
    }

    [[nodiscard]] OkxObSimdCodec& get_okx_ob_codec(int level) const
    {
        std::lock_guard lock(okx_ob_cache_mutex_);

        auto it = okx_ob_codecs_cache_.find(level);
        if (it == okx_ob_codecs_cache_.end()) {
            it = okx_ob_codecs_cache_.try_emplace(level, std::make_unique<ZstdCompressor>(level)).first;
        }
        return it->second;
    }

    [[nodiscard]] BinanceObSimdCodec& get_binance_ob_codec(int level) const
    {
        std::lock_guard lock(binance_ob_cache_mutex_);

        auto it = binance_ob_codecs_cache_.find(level);
        if (it == binance_ob_codecs_cache_.end()) {
            it = binance_ob_codecs_cache_.try_emplace(level, std::make_unique<ZstdCompressor>(level)).first;
        }
        return it->second;
    }

    [[nodiscard]] Temporal1dSimdCodec& get_t1d_codec(int level) const
    {
        std::lock_guard lock(t1d_cache_mutex_);

        auto it = t1d_codecs_cache_.find(level);
        if (it == t1d_codecs_cache_.end()) {
            it = t1d_codecs_cache_.try_emplace(level, std::make_unique<ZstdCompressor>(level)).first;
        }
        return it->second;
    }

    [[nodiscard]] DynamicTemporal2dSimdCodec& get_t2d_codec(size_t num_features, int level) const
    {
        const T2dCodecKey key = {num_features, level};
        std::lock_guard lock(t2d_cache_mutex_);

        auto it = t2d_codecs_cache_.find(key);
        if (it == t2d_codecs_cache_.end()) {
            it = t2d_codecs_cache_.try_emplace(key, num_features, std::make_unique<ZstdCompressor>(level)).first;
        }
        return it->second;
    }
};


// --- Public API Implementation ---

DataCompressor::DataCompressor() : pimpl_(std::make_unique<Impl>()) {}
DataCompressor::~DataCompressor() = default;
DataCompressor::DataCompressor(DataCompressor&&) noexcept = default;
DataCompressor& DataCompressor::operator=(DataCompressor&&) noexcept = default;


DataCompressor::ChunkResult DataCompressor::compress_zstd(
    std::span<const std::byte> data, std::span<const int64_t> shape, DType dtype, int level) const
{
    for (const auto dim : shape) {
        if (dim < 0) {
            return std::unexpected(CodecError{ErrorCode::InvalidChunkShape, "Shape dimensions cannot be negative."});
        }
    }

    // Zstd is simple and stateless enough to create on the stack without caching.
    ZstdCompressor compressor(level);
    auto compressed_result = compressor.compress(data);
    if (!compressed_result) {
        return std::unexpected(CodecError::from_string(compressed_result.error(), ErrorCode::CompressionFailure));
    }
    
    return create_chunk_from_result(std::move(*compressed_result),
                                    ChunkDataType::ZSTD_COMPRESSED,
                                    dtype,
                                    shape,
                                    ChunkFlags::ZSTD);
}

// --- Temporal 1D ---

DataCompressor::ChunkResult DataCompressor::compress_chunk(
    std::span<const float> data, ChunkDataType type, int level) const
{
    return compress_chunk(data, type, 0.0f, level);
}

DataCompressor::ChunkResult DataCompressor::compress_chunk(
    std::span<const int64_t> data, ChunkDataType type, int level) const
{
    return compress_chunk(data, type, 0, level);
}

DataCompressor::ChunkResult DataCompressor::compress_chunk(
    std::span<const float> data, ChunkDataType type, float prev_element, int level) const
{
    auto& codec = pimpl_->get_t1d_codec(level);
    std::lock_guard lock(pimpl_->temporal_1d_workspace_mutex_);
    
    std::expected<memory::vector<std::byte>, std::string> encoded_result;

    switch (type) {
        case ChunkDataType::TEMPORAL_1D_SIMD_F16_XOR_SHUFFLE_AS_F32:
            encoded_result = codec.encode16_Xor_Shuffle(data, prev_element, pimpl_->temporal_1d_workspace_);
            break;
        case ChunkDataType::TEMPORAL_1D_SIMD_F32_XOR_SHUFFLE:
            encoded_result = codec.encode32_Xor_Shuffle(data, prev_element, pimpl_->temporal_1d_workspace_);
            break;
        default:
            return std::unexpected(CodecError{ErrorCode::InvalidDataType, "Unsupported or mismatched chunk type for 1D float data."});
    }

    const int64_t shape_val = static_cast<int64_t>(data.size());
    return create_chunk_from_result(std::move(encoded_result),
        type, DType::FLOAT32, {&shape_val, 1}, ChunkFlags::NONE);
}

DataCompressor::ChunkResult DataCompressor::compress_chunk(
    std::span<const int64_t> data, ChunkDataType type, int64_t prev_element, int level) const
{
    auto& codec = pimpl_->get_t1d_codec(level);
    std::lock_guard lock(pimpl_->temporal_1d_workspace_mutex_);
    
    std::expected<memory::vector<std::byte>, std::string> encoded_result;

    switch (type) {
        case ChunkDataType::TEMPORAL_1D_SIMD_I64_XOR:
            encoded_result = codec.encode64_Xor(data, prev_element, pimpl_->temporal_1d_workspace_);
            break;
        case ChunkDataType::TEMPORAL_1D_SIMD_I64_DELTA:
            encoded_result = codec.encode64_Delta(data, prev_element, pimpl_->temporal_1d_workspace_);
            break;
        default:
            return std::unexpected(CodecError{ErrorCode::InvalidDataType, "Unsupported or mismatched chunk type for 1D int64 data."});
    }

    const int64_t shape_val = static_cast<int64_t>(data.size());
    return create_chunk_from_result(std::move(encoded_result),
        type, DType::INT64, {&shape_val, 1}, ChunkFlags::NONE);
}

DataCompressor::ChunkResult DataCompressor::compress_chunk(
    std::span<const float> data, ChunkDataType type, std::span<const int64_t> shape, std::span<const float> prev_state, int level) const
{
    std::expected<memory::vector<std::byte>, std::string> encoded_result;

    for (const auto dim : shape) {
        if (dim < 0) {
            return std::unexpected(CodecError{ErrorCode::InvalidChunkShape, "Shape dimensions cannot be negative."});
        }
    }

    switch (type) {
        case ChunkDataType::OKX_OB_SIMD_F16_AS_F32:
        case ChunkDataType::OKX_OB_SIMD_F32:
        {
            if (shape.size() != 3) return std::unexpected(CodecError{ErrorCode::InvalidChunkShape, "OKX Orderbook data requires a 3D shape."});
            if (static_cast<size_t>(shape[1]) != codecs::Orderbook::OKX_DEPTH || static_cast<size_t>(shape[2]) != codecs::Orderbook::OKX_FEATURES) {
                return std::unexpected(CodecError{ErrorCode::InvalidChunkShape, std::format("OKX orderbook shape mismatch. Expected ({}, {}), got ({}, {}).", codecs::Orderbook::OKX_DEPTH, codecs::Orderbook::OKX_FEATURES, shape[1], shape[2])});
            }
            auto& codec = pimpl_->get_okx_ob_codec(level);
            
            std::lock_guard lock(pimpl_->ob_workspace_mutex_); // Using generic OB workspace mutex for now
            std::remove_reference_t<decltype(codec)>::Snapshot snapshot;
            std::ranges::copy(prev_state, snapshot.begin());
            if (type == ChunkDataType::OKX_OB_SIMD_F16_AS_F32) {

                encoded_result = codec.encode16(data, snapshot, pimpl_->ob_workspace_);
            } else {
                encoded_result = codec.encode32(data, snapshot, pimpl_->ob_workspace_);
            }
            break;
        }
        case ChunkDataType::BINANCE_OB_SIMD_F16_AS_F32:
        case ChunkDataType::BINANCE_OB_SIMD_F32:
        {
            if (shape.size() != 3) return std::unexpected(CodecError{ErrorCode::InvalidChunkShape, "Binance Orderbook data requires a 3D shape."});
            if (static_cast<size_t>(shape[1]) != codecs::Orderbook::BINANCE_DEPTH || static_cast<size_t>(shape[2]) != codecs::Orderbook::BINANCE_FEATURES) {
                return std::unexpected(CodecError{ErrorCode::InvalidChunkShape, std::format("Binance orderbook shape mismatch. Expected ({}, {}), got ({}, {}).", codecs::Orderbook::BINANCE_DEPTH, codecs::Orderbook::BINANCE_FEATURES, shape[1], shape[2])});
            }
            auto& codec = pimpl_->get_binance_ob_codec(level);
            
            std::lock_guard lock(pimpl_->ob_workspace_mutex_); // Using generic OB workspace mutex for now
            std::remove_reference_t<decltype(codec)>::Snapshot snapshot;
            std::ranges::copy(prev_state, snapshot.begin());
            if (type == ChunkDataType::BINANCE_OB_SIMD_F16_AS_F32) {
                encoded_result = codec.encode16(data, snapshot, pimpl_->ob_workspace_);
            } else {
                encoded_result = codec.encode32(data, snapshot, pimpl_->ob_workspace_);
            }
            break;
        }
        case ChunkDataType::GENERIC_OB_SIMD_F16_AS_F32:
        case ChunkDataType::GENERIC_OB_SIMD_F32:
        {
            if (shape.size() != 3) return std::unexpected(CodecError{ErrorCode::InvalidChunkShape, "Orderbook data requires a 3D shape."});
            const size_t depth = static_cast<size_t>(shape[1]);
            const size_t features = static_cast<size_t>(shape[2]);
            auto& codec = pimpl_->get_ob_codec(depth, features, level);
            
            std::lock_guard lock(pimpl_->ob_workspace_mutex_);
            if (type == ChunkDataType::GENERIC_OB_SIMD_F16_AS_F32) {
                encoded_result = codec.encode16(data, prev_state, pimpl_->ob_workspace_);
            } else {
                encoded_result = codec.encode32(data, prev_state, pimpl_->ob_workspace_);
            }
            break;
        }
        case ChunkDataType::TEMPORAL_2D_SIMD_F16_AS_F32:
        case ChunkDataType::TEMPORAL_2D_SIMD_F32:
        {
            if (shape.size() != 2) return std::unexpected(CodecError{ErrorCode::InvalidChunkShape, "Temporal 2D data requires a 2D shape."});
            const size_t num_features = static_cast<size_t>(shape[1]);
            auto& codec = pimpl_->get_t2d_codec(num_features, level);

            std::lock_guard lock(pimpl_->temporal_2d_workspace_mutex_);
            if (type == ChunkDataType::TEMPORAL_2D_SIMD_F16_AS_F32) {
                encoded_result = codec.encode16(data, prev_state, pimpl_->temporal_2d_workspace_);
            } else {
                encoded_result = codec.encode32(data, prev_state, pimpl_->temporal_2d_workspace_);
            }
            break;
        }
        default:
            return std::unexpected(CodecError{ErrorCode::InvalidDataType, "Unsupported or mismatched chunk type for 2D/3D float data."});
    }

    return create_chunk_from_result(std::move(encoded_result), type, DType::FLOAT32, shape, ChunkFlags::NONE);
}

DataCompressor::ChunkResult DataCompressor::compress_chunk(
    std::span<const int64_t> data, ChunkDataType type, std::span<const int64_t> shape, std::span<const int64_t> prev_row, int level) const
{
    if (shape.size() != 2) return std::unexpected(CodecError{ErrorCode::InvalidChunkShape, "Temporal 2D int64 data requires a 2D shape."});
    
    for (const auto dim : shape) {
        if (dim < 0) {
            return std::unexpected(CodecError{ErrorCode::InvalidChunkShape, "Shape dimensions cannot be negative."});
        }
    }

    std::expected<memory::vector<std::byte>, std::string> encoded_result;

    switch (type) {
        case ChunkDataType::TEMPORAL_2D_SIMD_I64:
        {
            const size_t num_features = static_cast<size_t>(shape[1]);
            auto& codec = pimpl_->get_t2d_codec(num_features, level);
            
            std::lock_guard lock(pimpl_->temporal_2d_workspace_mutex_);
            encoded_result = codec.encode64(data, prev_row, pimpl_->temporal_2d_workspace_);
            break;
        }
        default:
            return std::unexpected(CodecError{ErrorCode::InvalidDataType, "Unsupported or mismatched chunk type for 2D int64 data."});
    }

    return create_chunk_from_result(std::move(encoded_result), type, DType::INT64, shape, ChunkFlags::NONE);
}

} // namespace cryptodd
