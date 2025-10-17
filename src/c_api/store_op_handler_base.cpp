#include "store_op_handler_base.h"
#include <numeric>
#include <bit>

namespace cryptodd::ffi {

using namespace utils;

std::expected<nlohmann::json, ExpectedError> StoreOperationHandlerBase::compress_and_write_chunk(
    CddContext& context,
    DataWriter& writer,
    const utils::DataSpec& data_spec,
    const nlohmann::json& encoding_json,
    std::span<const std::byte> chunk_input_data)
{
    auto codec_result = get_required<nlohmann::json>(encoding_json, "codec").and_then(parse_codec);
    if (!codec_result) return std::unexpected(codec_result.error());
    const auto codec = *codec_result;

    auto flags_result = parse_flags(encoding_json.value("flags", nlohmann::json::array()));
    if (!flags_result) return std::unexpected(flags_result.error());
    uint64_t flags = *flags_result;

    // Auto-set endianness if not specified
    if ((flags & (ChunkFlags::LITTLE_ENDIAN | ChunkFlags::BIG_ENDIAN)) == 0) {
        if constexpr (std::endian::native == std::endian::little) {
            flags |= ChunkFlags::LITTLE_ENDIAN;
        } else if constexpr (std::endian::native == std::endian::big) {
            flags |= ChunkFlags::BIG_ENDIAN;
        }
    }

    const int zstd_level = encoding_json.value("zstd_level", ZstdCompressor::DEFAULT_COMPRESSION_LEVEL);

    DataCompressor& compressor = context.get_compressor();
    DataCompressor::ChunkResult chunk_result;

    switch (codec) {
        case ChunkDataType::RAW: {
            auto append_result = writer.append_chunk(codec, data_spec.dtype, flags, data_spec.shape, chunk_input_data);
            if (!append_result) return std::unexpected(ExpectedError(append_result.error()));
            
            nlohmann::json result;
            result["chunk_index"] = *append_result;
            result["original_size"] = chunk_input_data.size_bytes();
            result["size"] = chunk_input_data.size_bytes();
            return result;
        }
        case ChunkDataType::ZSTD_COMPRESSED:
            chunk_result = compressor.compress_zstd(chunk_input_data, data_spec.shape, data_spec.dtype, zstd_level);
            flags |= ChunkFlags::ZSTD;
            break;
        // Cases for TEMPORAL_1D_...
        case ChunkDataType::TEMPORAL_1D_SIMD_F16_XOR_SHUFFLE_AS_F32:
        case ChunkDataType::TEMPORAL_1D_SIMD_F32_XOR_SHUFFLE:
            {
                if (data_spec.dtype != DType::FLOAT32) return std::unexpected(ExpectedError("This codec requires FLOAT32 dtype."));
                auto data_span = std::span(reinterpret_cast<const float*>(chunk_input_data.data()), chunk_input_data.size() / sizeof(float));
                chunk_result = compressor.compress_chunk(data_span, codec, 0.0f, zstd_level);
                break;
            }
        case ChunkDataType::TEMPORAL_1D_SIMD_I64_XOR:
        case ChunkDataType::TEMPORAL_1D_SIMD_I64_DELTA:
            {
                if (data_spec.dtype != DType::INT64) return std::unexpected(ExpectedError("This codec requires INT64 dtype."));
                auto data_span = std::span(reinterpret_cast<const int64_t*>(chunk_input_data.data()), chunk_input_data.size() / sizeof(int64_t));
                chunk_result = compressor.compress_chunk(data_span, codec, 0, zstd_level);
                break;
            }
        // Cases for OB and TEMPORAL_2D (float)
        case ChunkDataType::OKX_OB_SIMD_F16_AS_F32: case ChunkDataType::BINANCE_OB_SIMD_F16_AS_F32: case ChunkDataType::GENERIC_OB_SIMD_F16_AS_F32:
        case ChunkDataType::TEMPORAL_2D_SIMD_F16_AS_F32: case ChunkDataType::OKX_OB_SIMD_F32: case ChunkDataType::BINANCE_OB_SIMD_F32:
        case ChunkDataType::GENERIC_OB_SIMD_F32: case ChunkDataType::TEMPORAL_2D_SIMD_F32:
            {
                if (data_spec.dtype != DType::FLOAT32) return std::unexpected(ExpectedError("This codec requires FLOAT32 dtype."));
                auto data_span = std::span(reinterpret_cast<const float*>(chunk_input_data.data()), chunk_input_data.size() / sizeof(float));
                
                size_t prev_state_elements = (data_spec.shape.size() > 1) ? data_spec.shape[1] : 0;
                 if (data_spec.shape.size() == 3) prev_state_elements *= data_spec.shape[2];
                
                const auto zero_state_bytes = context.get_zero_state(prev_state_elements * sizeof(float));
                const std::span<const float> prev_state(reinterpret_cast<const float*>(zero_state_bytes.data()), prev_state_elements);

                chunk_result = compressor.compress_chunk(data_span, codec, data_spec.shape, prev_state, zstd_level);
                break;
            }
        // Case for TEMPORAL_2D (int64)
        case ChunkDataType::TEMPORAL_2D_SIMD_I64:
            {
                if (data_spec.dtype != DType::INT64) return std::unexpected(ExpectedError("This codec requires INT64 dtype."));
                auto data_span = std::span(reinterpret_cast<const int64_t*>(chunk_input_data.data()), chunk_input_data.size() / sizeof(int64_t));

                if (data_spec.shape.size() != 2) return std::unexpected(ExpectedError("TEMPORAL_2D_SIMD_I64 requires a 2D shape."));
                const size_t prev_state_elements = data_spec.shape[1];
                const auto zero_state_bytes = context.get_zero_state(prev_state_elements * sizeof(int64_t));
                const std::span<const int64_t> prev_state(reinterpret_cast<const int64_t*>(zero_state_bytes.data()), prev_state_elements);
                chunk_result = compressor.compress_chunk(data_span, codec, data_spec.shape, prev_state, zstd_level);
                break;
            }
        default:
            return std::unexpected(ExpectedError("The specified codec is not yet supported for writing."));
    }

    if (!chunk_result) return std::unexpected(ExpectedError(chunk_result.error().to_string()));

    auto& chunk = *chunk_result;
    auto append_result = writer.append_chunk(chunk->type(), chunk->dtype(), flags, chunk->get_shape(), chunk->data());
    if (!append_result) return std::unexpected(ExpectedError(append_result.error()));

    nlohmann::json result;
    result["chunk_index"] = *append_result;
    result["original_size"] = chunk_input_data.size_bytes();
    result["size"] = chunk->data().size();
    return result;
}

} // namespace cryptodd::ffi
