#include "data_extractor.h"

#include <format>
#include <map>
#include <mutex>
#include <optional>

#include "../codecs/codec_constants.h"
#include "../codecs/orderbook_simd_codec.h"
#include "../codecs/temporal_1d_simd_codec.h"
#include "../codecs/temporal_2d_simd_codec.h"
#include "../codecs/zstd_compressor.h"


namespace cryptodd
{

// PIMPL struct to hide implementation details
struct DataExtractor::Impl
{
    mutable std::unique_ptr<ZstdCompressor> zstd_;
    mutable std::once_flag zstd_init_flag_;

    // Static, fixed-dimension codecs
    mutable std::unique_ptr<OrderbookSimdCodec<codecs::Orderbook::OKX_DEPTH, codecs::Orderbook::OKX_FEATURES>> okx_ob_codec_;
    mutable std::once_flag okx_ob_codec_init_flag_;
    mutable std::unique_ptr<OrderbookSimdCodec<codecs::Orderbook::BINANCE_DEPTH, codecs::Orderbook::BINANCE_FEATURES>> binance_ob_codec_;
    mutable std::once_flag binance_ob_codec_init_flag_;
    mutable std::unique_ptr<Temporal1dSimdCodec> temporal_1d_codec_;
    mutable std::once_flag temporal_1d_codec_init_flag_;

    // Caches for dynamic-dimension codecs
    mutable std::map<std::tuple<size_t, size_t>, DynamicOrderbookSimdCodec> ob_codecs_;
    mutable std::mutex ob_codecs_mutex_;
    mutable std::map<size_t, DynamicTemporal2dSimdCodec> temporal_2d_codecs_;
    mutable std::mutex temporal_2d_codecs_mutex_;

    // Helper methods for lazy initialization
    [[nodiscard]] ZstdCompressor& get_zstd() const
    {
        std::call_once(zstd_init_flag_, [this]() { zstd_ = std::make_unique<ZstdCompressor>(); });
        return *zstd_;
    }

    [[nodiscard]] const OrderbookSimdCodec<codecs::Orderbook::OKX_DEPTH, codecs::Orderbook::OKX_FEATURES>& get_okx_ob_codec() const
    {
        std::call_once(okx_ob_codec_init_flag_,
                       [this]() { okx_ob_codec_ = std::make_unique<OrderbookSimdCodec<codecs::Orderbook::OKX_DEPTH, codecs::Orderbook::OKX_FEATURES>>(create_compressor()); });
        return *okx_ob_codec_;
    }

    [[nodiscard]] const OrderbookSimdCodec<codecs::Orderbook::BINANCE_DEPTH, codecs::Orderbook::BINANCE_FEATURES>& get_binance_ob_codec() const
    {
        std::call_once(binance_ob_codec_init_flag_,
                       [this]() { binance_ob_codec_ = std::make_unique<OrderbookSimdCodec<codecs::Orderbook::BINANCE_DEPTH, codecs::Orderbook::BINANCE_FEATURES>>(create_compressor()); });
        return *binance_ob_codec_;
    }

    [[nodiscard]] DynamicOrderbookSimdCodec& get_ob_codec(const size_t depth, const size_t features)
    {
        const std::tuple<size_t, size_t> shape = {depth, features};
        std::lock_guard lock(ob_codecs_mutex_);
        auto it = ob_codecs_.find(shape);
        if (it == ob_codecs_.end())
        {
            it = ob_codecs_.try_emplace(shape, depth, features, create_compressor()).first;
        }
        return it->second;
    }

    [[nodiscard]] const Temporal1dSimdCodec& get_temporal_1d_codec() const
    {
        std::call_once(temporal_1d_codec_init_flag_,
                       [this]() { temporal_1d_codec_ = std::make_unique<Temporal1dSimdCodec>(create_compressor()); });
        return *temporal_1d_codec_;
    }

    [[nodiscard]] DynamicTemporal2dSimdCodec& get_temporal_2d_codec(const size_t num_features)
    {
        std::lock_guard lock(temporal_2d_codecs_mutex_);
        auto it = temporal_2d_codecs_.find(num_features);
        if (it == temporal_2d_codecs_.end())
        {
            it = temporal_2d_codecs_.try_emplace(num_features, num_features, create_compressor()).first;
        }
        return it->second;
    }

    static std::unique_ptr<ICompressor> create_compressor()
    {
        // By default, we use a moderate compression level.
        return std::make_unique<ZstdCompressor>(ZstdCompressor::DEFAULT_COMPRESSION_LEVEL);
    }

    // Private handlers for different chunk types
    [[nodiscard]] DataExtractor::BufferResult handle_zstd_chunk(std::unique_ptr<Buffer> buffer) const
    {
        auto decompressed_result = get_zstd().decompress(buffer->as_bytes());
        if (!decompressed_result) return std::unexpected(CodecError::from_string(decompressed_result.error(), ErrorCode::DecompressionFailure));
        return std::make_unique<Buffer>(std::move(*decompressed_result));
    }

    [[nodiscard]] DataExtractor::BufferResult handle_orderbook_chunk(const Chunk& chunk,
                                                                       std::unique_ptr<Buffer> buffer,
                                                                       std::span<float> prev_snapshot_state)
    {
        if (chunk.get_shape().size() < 3)
        {
            return std::unexpected(CodecError{ErrorCode::InvalidChunkShape, std::format("Orderbook chunk must have at least 3 dimensions, but got {}.", chunk.get_shape().size())});
        }
        if (chunk.dtype() != DType::FLOAT32)
        {
            return std::unexpected(CodecError{ErrorCode::InvalidDataType, "Orderbook chunk must have a dtype of FLOAT32."});
        }

        const size_t num_snapshots = chunk.get_shape()[0];
        const size_t depth = chunk.get_shape()[1];
        const size_t features = chunk.get_shape()[2];

        std::expected<Float32AlignedVector, CodecError> decoded_result;

        if (chunk.type() == ChunkDataType::OKX_OB_SIMD_F16_AS_F32 || chunk.type() == ChunkDataType::OKX_OB_SIMD_F32)
        {
            if (depth != codecs::Orderbook::OKX_DEPTH || features != codecs::Orderbook::OKX_FEATURES)
            {
                return std::unexpected(CodecError{ErrorCode::InvalidChunkShape, std::format("OKX orderbook shape mismatch. Expected ({}, {}), got ({}, {}).", codecs::Orderbook::OKX_DEPTH, codecs::Orderbook::OKX_FEATURES, depth, features)});
            }
            auto& codec = get_okx_ob_codec();
            OrderbookSimdCodec<codecs::Orderbook::OKX_DEPTH, codecs::Orderbook::OKX_FEATURES>::Snapshot prev_snapshot_arr;
            if(prev_snapshot_state.size() != prev_snapshot_arr.size()) return std::unexpected(CodecError{ErrorCode::InvalidStateSize, "OKX prev_snapshot_state size mismatch"});
            std::copy_n(prev_snapshot_state.begin(), prev_snapshot_arr.size(), prev_snapshot_arr.begin());

            auto res = (chunk.type() == ChunkDataType::OKX_OB_SIMD_F16_AS_F32)
                           ? codec.decode16(buffer->as_bytes(), num_snapshots, prev_snapshot_arr)
                           : codec.decode32(buffer->as_bytes(), num_snapshots, prev_snapshot_arr);
            if (!res) return std::unexpected(CodecError::from_string(res.error()));
            std::copy_n(prev_snapshot_arr.begin(), prev_snapshot_state.size(), prev_snapshot_state.begin());
            decoded_result = std::move(*res);
        }
        else if (chunk.type() == ChunkDataType::BINANCE_OB_SIMD_F16_AS_F32 || chunk.type() == ChunkDataType::BINANCE_OB_SIMD_F32)
        {
            if (depth != codecs::Orderbook::BINANCE_DEPTH || features != codecs::Orderbook::BINANCE_FEATURES)
            {
                return std::unexpected(CodecError{ErrorCode::InvalidChunkShape, std::format("Binance orderbook shape mismatch. Expected ({}, {}), got ({}, {}).", codecs::Orderbook::BINANCE_DEPTH, codecs::Orderbook::BINANCE_FEATURES, depth, features)});
            }
            auto& codec = get_binance_ob_codec();
            OrderbookSimdCodec<codecs::Orderbook::BINANCE_DEPTH, codecs::Orderbook::BINANCE_FEATURES>::Snapshot prev_snapshot_arr;
            if(prev_snapshot_state.size() != prev_snapshot_arr.size()) return std::unexpected(CodecError{ErrorCode::InvalidStateSize, "Binance prev_snapshot_state size mismatch"});
            std::copy_n(prev_snapshot_state.begin(), prev_snapshot_arr.size(), prev_snapshot_arr.begin());

            auto res = (chunk.type() == ChunkDataType::BINANCE_OB_SIMD_F16_AS_F32)
                           ? codec.decode16(buffer->as_bytes(), num_snapshots, prev_snapshot_arr)
                           : codec.decode32(buffer->as_bytes(), num_snapshots, prev_snapshot_arr);
            if (!res) return std::unexpected(CodecError::from_string(res.error()));
            std::copy_n(prev_snapshot_arr.begin(), prev_snapshot_state.size(), prev_snapshot_state.begin());
            decoded_result = std::move(*res);
        }
        else // GENERIC_OB_SIMD
        {
            auto& codec = get_ob_codec(depth, features);
            auto res = (chunk.type() == ChunkDataType::GENERIC_OB_SIMD_F16_AS_F32)
                           ? codec.decode16(buffer->as_bytes(), num_snapshots, prev_snapshot_state)
                           : codec.decode32(buffer->as_bytes(), num_snapshots, prev_snapshot_state);
            if (!res) return std::unexpected(CodecError::from_string(res.error()));
            decoded_result = std::move(*res);
        }

        if (!decoded_result)
        {
            return std::unexpected(decoded_result.error()); // This should not happen if res is checked
        }

        return std::make_unique<Buffer>(std::move(*decoded_result));
    }

    [[nodiscard]] DataExtractor::BufferResult handle_temporal_1d_chunk(const Chunk& chunk, std::unique_ptr<Buffer> buffer, float& prev_element)
    {
        if (chunk.get_shape().size() != 1)
        {
            return std::unexpected(CodecError{ErrorCode::InvalidChunkShape, std::format("Temporal 1D chunk must have 1 dimension, but got {}.", chunk.get_shape().size())});
        }
        const size_t num_elements = chunk.num_elements();
        auto& codec = get_temporal_1d_codec();

        switch (chunk.type())
        {
        case ChunkDataType::TEMPORAL_1D_SIMD_F16_XOR_SHUFFLE_AS_F32:
            {
                if (chunk.dtype() != DType::FLOAT32) return std::unexpected(CodecError{ErrorCode::InvalidDataType, "Expected FLOAT32 dtype for TEMPORAL_1D_SIMD_F16_XOR_SHUFFLE_AS_F32."});
                auto result = codec.decode16_Xor_Shuffle(buffer->as_bytes(), num_elements, prev_element);
                if (!result) return std::unexpected(CodecError::from_string(result.error()));
                return std::make_unique<Buffer>(std::move(*result));
            }
        case ChunkDataType::TEMPORAL_1D_SIMD_F32_XOR_SHUFFLE:
            {
                if (chunk.dtype() != DType::FLOAT32) return std::unexpected(CodecError{ErrorCode::InvalidDataType, "Expected FLOAT32 dtype for TEMPORAL_1D_SIMD_F32_XOR_SHUFFLE."});
                auto result = codec.decode32_Xor_Shuffle(buffer->as_bytes(), num_elements, prev_element);
                if (!result) return std::unexpected(CodecError::from_string(result.error()));
                return std::make_unique<Buffer>(std::move(*result));
            }
        default:
            return std::unexpected(CodecError{ErrorCode::InvalidDataType, "Chunk type does not match float state for 1D temporal codec."});
        }
    }

    [[nodiscard]] DataExtractor::BufferResult handle_temporal_1d_chunk(const Chunk& chunk, std::unique_ptr<Buffer> buffer, int64_t& prev_element)
    {
        if (chunk.get_shape().size() != 1)
        {
            return std::unexpected(CodecError{ErrorCode::InvalidChunkShape, std::format("Temporal 1D chunk must have 1 dimension, but got {}.", chunk.get_shape().size())});
        }
        const size_t num_elements = chunk.num_elements();
        auto& codec = get_temporal_1d_codec();

        switch (chunk.type())
        {
        case ChunkDataType::TEMPORAL_1D_SIMD_I64_XOR:
            {
                if (chunk.dtype() != DType::INT64) return std::unexpected(CodecError{ErrorCode::InvalidDataType, "Expected INT64 dtype for TEMPORAL_1D_SIMD_I64_XOR."});
                auto result = codec.decode64_Xor(buffer->as_bytes(), num_elements, prev_element);
                if (!result) return std::unexpected(CodecError::from_string(result.error()));
                return std::make_unique<Buffer>(std::move(*result));
            }
        case ChunkDataType::TEMPORAL_1D_SIMD_I64_DELTA:
            {
                if (chunk.dtype() != DType::INT64) return std::unexpected(CodecError{ErrorCode::InvalidDataType, "Expected INT64 dtype for TEMPORAL_1D_SIMD_I64_DELTA."});
                auto result = codec.decode64_Delta(buffer->as_bytes(), num_elements, prev_element);
                if (!result) return std::unexpected(CodecError::from_string(result.error()));
                return std::make_unique<Buffer>(std::move(*result));
            }
        default:
            return std::unexpected(CodecError{ErrorCode::InvalidDataType, "Chunk type does not match int64 state for 1D temporal codec."});
        }
    }

    [[nodiscard]] DataExtractor::BufferResult handle_temporal_2d_chunk(const Chunk& chunk, std::unique_ptr<Buffer> buffer, std::span<float> prev_row)
    {
        if (chunk.get_shape().size() != 2)
        {
            return std::unexpected(CodecError{ErrorCode::InvalidChunkShape, std::format("Temporal 2D chunk must have 2 dimensions, but got {}.", chunk.get_shape().size())});
        }
        const size_t num_features = chunk.get_shape()[1];
        if (prev_row.size() != num_features)
        {
            return std::unexpected(CodecError{ErrorCode::InvalidStateSize, std::format("Previous row size mismatch. Expected {}, got {}.", num_features, prev_row.size())});
        }

        auto& codec = get_temporal_2d_codec(num_features);

        switch (chunk.type())
        {
        case ChunkDataType::TEMPORAL_2D_SIMD_F16_AS_F32:
            {
                if (chunk.dtype() != DType::FLOAT32) return std::unexpected(CodecError{ErrorCode::InvalidDataType, "Expected FLOAT32 dtype for TEMPORAL_2D_SIMD_F16_AS_F32."});
                auto result = codec.decode16(buffer->as_bytes(), prev_row);
                if (!result) return std::unexpected(CodecError::from_string(result.error()));
                return std::make_unique<Buffer>(std::move(*result));
            }
        case ChunkDataType::TEMPORAL_2D_SIMD_F32:
            {
                if (chunk.dtype() != DType::FLOAT32) return std::unexpected(CodecError{ErrorCode::InvalidDataType, "Expected FLOAT32 dtype for TEMPORAL_2D_SIMD_F32."});
                auto result = codec.decode32(buffer->as_bytes(), prev_row);
                if (!result) return std::unexpected(CodecError::from_string(result.error()));
                return std::make_unique<Buffer>(std::move(*result));
            }
        default:
            return std::unexpected(CodecError{ErrorCode::InvalidDataType, "Chunk type does not match float state for 2D temporal codec."});
        }
    }

    [[nodiscard]] DataExtractor::BufferResult handle_temporal_2d_chunk(const Chunk& chunk, std::unique_ptr<Buffer> buffer, std::span<int64_t> prev_row)
    {
        if (chunk.get_shape().size() != 2)
        {
            return std::unexpected(CodecError{ErrorCode::InvalidChunkShape, std::format("Temporal 2D chunk must have 2 dimensions, but got {}.", chunk.get_shape().size())});
        }
        const size_t num_features = chunk.get_shape()[1];
        if (prev_row.size() != num_features)
        {
            return std::unexpected(CodecError{ErrorCode::InvalidStateSize, std::format("Previous row size mismatch. Expected {}, got {}.", num_features, prev_row.size())});
        }

        auto& codec = get_temporal_2d_codec(num_features);

        switch (chunk.type())
        {
        case ChunkDataType::TEMPORAL_2D_SIMD_I64:
            {
                if (chunk.dtype() != DType::INT64) return std::unexpected(CodecError{ErrorCode::InvalidDataType, "Expected INT64 dtype for TEMPORAL_2D_SIMD_I64."});
                auto result = codec.decode64(buffer->as_bytes(), prev_row);
                if (!result) return std::unexpected(CodecError::from_string(result.error()));
                return std::make_unique<Buffer>(std::move(*result));
            }
        default:
            return std::unexpected(CodecError{ErrorCode::InvalidDataType, "Chunk type does not match int64 state for 2D temporal codec."});
        }
    }
};

DataExtractor::DataExtractor() : pimpl_(std::make_unique<Impl>()) {}
DataExtractor::~DataExtractor() = default;
DataExtractor::DataExtractor(DataExtractor&&) noexcept = default;
DataExtractor& DataExtractor::operator=(DataExtractor&&) noexcept = default;

DataExtractor::BufferResult DataExtractor::read_chunk(Chunk& chunk)
{
    // The initial buffer contains the raw (potentially compressed) data from the chunk.
    auto buffer = std::make_unique<Buffer>(std::move(chunk.data()));

    switch (chunk.type())
    {
    case ChunkDataType::RAW:
        return buffer;

    case ChunkDataType::ZSTD_COMPRESSED:
        return pimpl_->handle_zstd_chunk(std::move(buffer));

    case ChunkDataType::OKX_OB_SIMD_F16_AS_F32:
    case ChunkDataType::OKX_OB_SIMD_F32:
    case ChunkDataType::BINANCE_OB_SIMD_F16_AS_F32:
    case ChunkDataType::BINANCE_OB_SIMD_F32:
    case ChunkDataType::GENERIC_OB_SIMD_F16_AS_F32:
    case ChunkDataType::GENERIC_OB_SIMD_F32:
        {
            const size_t snapshot_size = chunk.get_shape()[1] * chunk.get_shape()[2];
            memory::vector<float> prev_snapshot(snapshot_size, 0.0f);
            return pimpl_->handle_orderbook_chunk(chunk, std::move(buffer), prev_snapshot);
        }

    case ChunkDataType::TEMPORAL_1D_SIMD_F16_XOR_SHUFFLE_AS_F32:
    case ChunkDataType::TEMPORAL_1D_SIMD_F32_XOR_SHUFFLE:
        {
            float prev_element = 0.0f;
            return pimpl_->handle_temporal_1d_chunk(chunk, std::move(buffer), prev_element);
        }

    case ChunkDataType::TEMPORAL_1D_SIMD_I64_XOR:
    case ChunkDataType::TEMPORAL_1D_SIMD_I64_DELTA:
        {
            int64_t prev_element = 0;
            return pimpl_->handle_temporal_1d_chunk(chunk, std::move(buffer), prev_element);
        }

    case ChunkDataType::TEMPORAL_2D_SIMD_F16_AS_F32:
    case ChunkDataType::TEMPORAL_2D_SIMD_F32:
        {
            const size_t num_features = chunk.get_shape()[1];
            memory::vector<float> prev_row(num_features, 0.0f);
            return pimpl_->handle_temporal_2d_chunk(chunk, std::move(buffer), prev_row);
        }

    case ChunkDataType::TEMPORAL_2D_SIMD_I64:
        {
            const size_t num_features = chunk.get_shape()[1];
            memory::vector<int64_t> prev_row(num_features, 0);
            return pimpl_->handle_temporal_2d_chunk(chunk, std::move(buffer), prev_row);
        }

    default:
        return std::unexpected(CodecError{ErrorCode::Unknown, std::format("Unknown or unsupported chunk type for extraction: {}", static_cast<int>(chunk.type()))});
    }
}

DataExtractor::BufferResult DataExtractor::read_chunk(Chunk& chunk, float& prev_element)
{
    auto buffer = std::make_unique<Buffer>(std::move(chunk.data()));
    return pimpl_->handle_temporal_1d_chunk(chunk, std::move(buffer), prev_element);
}

DataExtractor::BufferResult DataExtractor::read_chunk(Chunk& chunk, int64_t& prev_element)
{
    auto buffer = std::make_unique<Buffer>(std::move(chunk.data()));
    return pimpl_->handle_temporal_1d_chunk(chunk, std::move(buffer), prev_element);
}

DataExtractor::BufferResult DataExtractor::read_chunk(Chunk& chunk, std::span<float> prev_row)
{
    switch (chunk.type())
    {
    case ChunkDataType::TEMPORAL_2D_SIMD_F16_AS_F32:
    case ChunkDataType::TEMPORAL_2D_SIMD_F32:
        {
            auto buffer = std::make_unique<Buffer>(std::move(chunk.data()));
            return pimpl_->handle_temporal_2d_chunk(chunk, std::move(buffer), prev_row);
        }
    case ChunkDataType::OKX_OB_SIMD_F16_AS_F32:
    case ChunkDataType::OKX_OB_SIMD_F32:
    case ChunkDataType::BINANCE_OB_SIMD_F16_AS_F32:
    case ChunkDataType::BINANCE_OB_SIMD_F32:
    case ChunkDataType::GENERIC_OB_SIMD_F16_AS_F32:
    case ChunkDataType::GENERIC_OB_SIMD_F32:
        {
            // This overload is for both 2D and 3D, so we must forward to the correct implementation.
            return pimpl_->handle_orderbook_chunk(chunk, std::make_unique<Buffer>(std::move(chunk.data())), prev_row);
        }
    default:
        return std::unexpected(CodecError{ErrorCode::InvalidDataType, "Chunk type does not match a known float-span stateful codec."});
    }
}

DataExtractor::BufferResult DataExtractor::read_chunk(Chunk& chunk, std::span<int64_t> prev_row)
{
    auto buffer = std::make_unique<Buffer>(std::move(chunk.data()));
    return pimpl_->handle_temporal_2d_chunk(chunk, std::move(buffer), prev_row);
}

} // namespace cryptodd
