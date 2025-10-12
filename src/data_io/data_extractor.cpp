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

    mutable std::unique_ptr<Temporal1dSimdCodecWorkspace> workspace_1d_;
    mutable std::once_flag workspace_1d_init_flag_;
    mutable std::unique_ptr<Temporal2dSimdCodecWorkspace> workspace_2d_;
    mutable std::once_flag workspace_2d_init_flag_;
    mutable std::unique_ptr<OrderbookSimdCodecWorkspace> workspace_ob_;
    mutable std::once_flag workspace_ob_init_flag_;

    mutable std::unique_ptr<OrderbookSimdCodec<codecs::Orderbook::OKX_DEPTH, codecs::Orderbook::OKX_FEATURES>> okx_ob_codec_;
    mutable std::once_flag okx_ob_codec_init_flag_;
    mutable std::unique_ptr<OrderbookSimdCodec<codecs::Orderbook::BINANCE_DEPTH, codecs::Orderbook::BINANCE_FEATURES>> binance_ob_codec_;
    mutable std::once_flag binance_ob_codec_init_flag_;

    mutable std::map<std::tuple<size_t, size_t>, DynamicOrderbookSimdCodec> ob_codecs_;
    mutable std::mutex ob_codecs_mutex_;

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

    [[nodiscard]] DynamicOrderbookSimdCodec& get_ob_codec(size_t depth, size_t features)
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

    [[nodiscard]] OrderbookSimdCodecWorkspace& get_ob_workspace() const
    {
        std::call_once(workspace_ob_init_flag_, [this]() { workspace_ob_ = std::make_unique<OrderbookSimdCodecWorkspace>(); });
        return *workspace_ob_;
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
        if (!decompressed_result)
        {
            return std::unexpected(decompressed_result.error());
        }
        return std::make_unique<Buffer>(std::move(*decompressed_result));
    }

    [[nodiscard]] DataExtractor::BufferResult handle_orderbook_chunk(const Chunk& chunk,
                                                                       std::unique_ptr<Buffer> buffer)
    {
        if (chunk.get_shape().size() < 3)
        {
            return std::unexpected(std::format("Orderbook chunk must have at least 3 dimensions, but got {}.",
                                               chunk.get_shape().size()));
        }
        if (chunk.dtype() != DType::FLOAT32)
        {
            return std::unexpected("Orderbook chunk must have a dtype of FLOAT32.");
        }

        const size_t num_snapshots = chunk.get_shape()[0];
        const size_t depth = chunk.get_shape()[1];
        const size_t features = chunk.get_shape()[2];

        std::expected<std::vector<float>, std::string> decoded_result;

        if (chunk.type() == ChunkDataType::OKX_OB_SIMD_F16 || chunk.type() == ChunkDataType::OKX_OB_SIMD_F32)
        {
            if (depth != codecs::Orderbook::OKX_DEPTH || features != codecs::Orderbook::OKX_FEATURES)
            {
                return std::unexpected(std::format("OKX orderbook shape mismatch. Expected ({}, {}), got ({}, {}).",
                                                   codecs::Orderbook::OKX_DEPTH, codecs::Orderbook::OKX_FEATURES, depth, features));
            }
            auto& codec = get_okx_ob_codec();
            OrderbookSimdCodec<codecs::Orderbook::OKX_DEPTH, codecs::Orderbook::OKX_FEATURES>::Snapshot prev_snapshot{}; // Start with a zeroed-out previous state
            decoded_result = (chunk.type() == ChunkDataType::OKX_OB_SIMD_F16)
                                 ? codec.decode16(buffer->as_bytes(), num_snapshots, prev_snapshot)
                                 : codec.decode32(buffer->as_bytes(), num_snapshots, prev_snapshot);
        }
        else if (chunk.type() == ChunkDataType::BINANCE_OB_SIMD_F16 || chunk.type() == ChunkDataType::BINANCE_OB_SIMD_F32)
        {
            if (depth != codecs::Orderbook::BINANCE_DEPTH || features != codecs::Orderbook::BINANCE_FEATURES)
            {
                return std::unexpected(std::format("Binance orderbook shape mismatch. Expected ({}, {}), got ({}, {}).",
                                                   codecs::Orderbook::BINANCE_DEPTH, codecs::Orderbook::BINANCE_FEATURES, depth,
                                                   features));
            }
            auto& codec = get_binance_ob_codec();
            OrderbookSimdCodec<codecs::Orderbook::BINANCE_DEPTH, codecs::Orderbook::BINANCE_FEATURES>::Snapshot prev_snapshot{}; // Start with a zeroed-out previous state
            decoded_result = (chunk.type() == ChunkDataType::BINANCE_OB_SIMD_F16)
                                 ? codec.decode16(buffer->as_bytes(), num_snapshots, prev_snapshot)
                                 : codec.decode32(buffer->as_bytes(), num_snapshots, prev_snapshot);
        }
        else // GENERIC_OB_SIMD
        {
            auto& codec = get_ob_codec(depth, features);
            std::vector<float> prev_snapshot(codec.get_snapshot_size(), 0.0f); // Start with a zeroed-out previous state
            decoded_result = (chunk.type() == ChunkDataType::GENERIC_OB_SIMD_F16)
                                 ? codec.decode16(buffer->as_bytes(), num_snapshots, prev_snapshot)
                                 : codec.decode32(buffer->as_bytes(), num_snapshots, prev_snapshot);
        }

        if (!decoded_result)
        {
            return std::unexpected(decoded_result.error());
        }

        return std::make_unique<Buffer>(std::move(*decoded_result));
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

    case ChunkDataType::OKX_OB_SIMD_F16:
    case ChunkDataType::OKX_OB_SIMD_F32:
    case ChunkDataType::BINANCE_OB_SIMD_F16:
    case ChunkDataType::BINANCE_OB_SIMD_F32:
    case ChunkDataType::GENERIC_OB_SIMD_F16:
    case ChunkDataType::GENERIC_OB_SIMD_F32:
        return pimpl_->handle_orderbook_chunk(chunk, std::move(buffer));

    default:
        return std::unexpected(std::format("Unknown or unsupported chunk type for extraction: {}",
                                           static_cast<int>(chunk.type())));
    }
}
} // namespace cryptodd