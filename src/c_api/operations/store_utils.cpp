#define MAGIC_ENUM_ENABLE_HASH 1
#include <magic_enum/magic_enum.hpp>
#include <numeric> // For std::accumulate
#include <stdexcept> // For std::runtime_error
#include <bit> // For std::endian

#include "../operations/store_utils.h"
#include "../../codecs/zstd_compressor.h"
#include "../../data_io/data_writer.h"
#include "../../data_io/data_compressor.h"
#include "../../file_format/cdd_file_format.h" // For get_dtype_size
#include "../file_format/blake3_stream_hasher.h"

namespace cryptodd::ffi::StoreUtils {

std::expected<ChunkWriteDetails, ExpectedError> compress_and_write_chunk(
    CddContext& context,
    cryptodd::DataWriter& writer,
    const DataSpec& data_spec,
    const EncodingSpec& encoding_spec,
    std::span<const std::byte> chunk_input_data)
{
    cryptodd::ChunkDataType codec = encoding_spec.codec;
    uint64_t flags = 0;
    for (const auto& flag_str : encoding_spec.flags) {
        if (auto flag = magic_enum::enum_cast<cryptodd::ChunkFlags>(flag_str); flag.has_value()) {
            flags |= static_cast<uint64_t>(flag.value());
        } else {
            return std::unexpected(ExpectedError("Unknown flag: " + flag_str));
        }
    }

    if ((flags & (cryptodd::ChunkFlags::LITTLE_ENDIAN | cryptodd::ChunkFlags::BIG_ENDIAN)) == 0) {
        if constexpr (std::endian::native == std::endian::little) {
            flags |= cryptodd::ChunkFlags::LITTLE_ENDIAN;
        } else if constexpr (std::endian::native == std::endian::big) {
            flags |= cryptodd::ChunkFlags::BIG_ENDIAN;
        }
    }

    const int zstd_level = encoding_spec.zstd_level.value_or(cryptodd::ZstdCompressor::DEFAULT_COMPRESSION_LEVEL);

    cryptodd::DataCompressor& compressor = context.get_compressor();
    
    std::expected<size_t, std::string> append_result;
    size_t compressed_size = 0;
    size_t original_size = chunk_input_data.size();
    const blake3_hash256_t raw_data_hash = calculate_blake3_hash256(chunk_input_data);

    if (codec != cryptodd::ChunkDataType::RAW) {
        std::remove_reference_t<decltype(compressor)>::ChunkResult chunk_result;
        switch (codec) {
            case cryptodd::ChunkDataType::ZSTD_COMPRESSED:
                chunk_result = compressor.compress_zstd(chunk_input_data, data_spec.shape, data_spec.dtype, zstd_level);
                flags |= cryptodd::ChunkFlags::ZSTD;
                break;
            case cryptodd::ChunkDataType::TEMPORAL_1D_SIMD_F16_XOR_SHUFFLE_AS_F32:
            case cryptodd::ChunkDataType::TEMPORAL_1D_SIMD_F32_XOR_SHUFFLE:
                {
                    if (data_spec.dtype != cryptodd::DType::FLOAT32) return std::unexpected(ExpectedError("This codec requires FLOAT32 dtype."));
                    auto data_span = std::span(reinterpret_cast<const float*>(chunk_input_data.data()), chunk_input_data.size() / sizeof(float));
                    chunk_result = compressor.compress_chunk(data_span, codec, 0.0f, zstd_level);
                    break;
                }
            case cryptodd::ChunkDataType::TEMPORAL_1D_SIMD_I64_XOR:
            case cryptodd::ChunkDataType::TEMPORAL_1D_SIMD_I64_DELTA:
                {
                    if (data_spec.dtype != cryptodd::DType::INT64) return std::unexpected(ExpectedError("This codec requires INT64 dtype."));
                    auto data_span = std::span(reinterpret_cast<const int64_t*>(chunk_input_data.data()), chunk_input_data.size() / sizeof(int64_t));
                    chunk_result = compressor.compress_chunk(data_span, codec, 0, zstd_level);
                    break;
                }
            case cryptodd::ChunkDataType::OKX_OB_SIMD_F16_AS_F32: case cryptodd::ChunkDataType::BINANCE_OB_SIMD_F16_AS_F32: case cryptodd::ChunkDataType::GENERIC_OB_SIMD_F16_AS_F32:
            case cryptodd::ChunkDataType::TEMPORAL_2D_SIMD_F16_AS_F32: case cryptodd::ChunkDataType::OKX_OB_SIMD_F32: case cryptodd::ChunkDataType::BINANCE_OB_SIMD_F32:
            case cryptodd::ChunkDataType::GENERIC_OB_SIMD_F32: case cryptodd::ChunkDataType::TEMPORAL_2D_SIMD_F32:
                {
                    if (data_spec.dtype != cryptodd::DType::FLOAT32) return std::unexpected(ExpectedError("This codec requires FLOAT32 dtype."));
                    auto data_span = std::span(reinterpret_cast<const float*>(chunk_input_data.data()), chunk_input_data.size() / sizeof(float));
                    
                    size_t prev_state_elements = (data_spec.shape.size() > 1) ? data_spec.shape[1] : 0;
                    if (data_spec.shape.size() == 3) prev_state_elements *= data_spec.shape[2];
                    
                    const auto zero_state_bytes = context.get_zero_state(prev_state_elements * sizeof(float));
                    const std::span<const float> prev_state(reinterpret_cast<const float*>(zero_state_bytes.data()), prev_state_elements);

                    chunk_result = compressor.compress_chunk(data_span, codec, data_spec.shape, prev_state, zstd_level);
                    break;
                }
            case cryptodd::ChunkDataType::TEMPORAL_2D_SIMD_I64:
                {
                    if (data_spec.dtype != cryptodd::DType::INT64) return std::unexpected(ExpectedError("This codec requires INT64 dtype."));
                    auto data_span = std::span(reinterpret_cast<const int64_t*>(chunk_input_data.data()), chunk_input_data.size() / sizeof(int64_t));

                    if (data_spec.shape.size() != 2) return std::unexpected(ExpectedError("TEMPORAL_2D_SIMD_I64 requires a 2D shape."));
                    const size_t prev_state_elements = data_spec.shape[1];
                    const auto zero_state_bytes = context.get_zero_state(prev_state_elements * sizeof(int64_t));
                    const std::span<const int64_t> prev_state(reinterpret_cast<const int64_t*>(zero_state_bytes.data()), prev_state_elements);
                    chunk_result = compressor.compress_chunk(data_span, codec, data_spec.shape, prev_state, zstd_level);
                    break;
                }
            default:
                return std::unexpected(ExpectedError("The specified codec is not RAW and not a supported compression type for writing."));
        }
        if (!chunk_result) return std::unexpected(ExpectedError(chunk_result.error().to_string()));
        
        auto& chunk = *chunk_result->get();
        compressed_size = chunk.data().size(); // Get size BEFORE data is moved inside append_chunk.
        append_result = writer.append_chunk(chunk.type(), chunk.dtype(), flags, chunk.get_shape(), chunk, raw_data_hash);
    } else {
        // For raw data, a temporary chunk must be created to hold the data for the writer.
        Chunk temp_chunk;
        temp_chunk.set_data({chunk_input_data.begin(), chunk_input_data.end()});
        compressed_size = chunk_input_data.size();
        append_result = writer.append_chunk(codec, data_spec.dtype, flags, data_spec.shape, temp_chunk, raw_data_hash);
    }

    if (!append_result) return std::unexpected(ExpectedError(append_result.error()));

    float ratio = (original_size == 0) ? 1.0f : static_cast<float>(compressed_size) / original_size;

    return ChunkWriteDetails {
        .chunk_index = *append_result,
        .original_size = static_cast<int64_t>(original_size),
        .compressed_size = static_cast<int64_t>(compressed_size),
        .compression_ratio = ratio
    };
}

} // namespace cryptodd::ffi::StoreUtils
