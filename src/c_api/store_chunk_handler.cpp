#include "store_chunk_handler.h"
#include <bit> // For std::endian
#include <numeric>
#include "../c_api/c_api_utils.h"
#include "../data_io/data_compressor.h"

namespace cryptodd::ffi
{

    using namespace utils;

    std::span<const std::byte> CddContext::get_zero_state(size_t byte_size) {
        std::lock_guard lock(zero_state_cache_mutex_);
        auto it = zero_state_cache_.find(byte_size);
        if (it == zero_state_cache_.end()) {
            it = zero_state_cache_.try_emplace(byte_size, byte_size, std::byte{0}).first;
        }
        return it->second;
    }




    std::expected<nlohmann::json, ExpectedError> StoreChunkHandler::execute(CddContext& context,
                                                                            const nlohmann::json& op_request,
                                                                            std::span<const std::byte> input_data,
                                                                            std::span<const std::byte> )
    {

        auto writer_opt = context.get_writer();
        if (!writer_opt)
        {
            return std::unexpected(ExpectedError("Context is not in a writable mode."));
        }
        DataWriter& writer = writer_opt.value().get();

        // 2. Parse JSON specifications
        auto data_spec_result = get_required<nlohmann::json>(op_request, "data_spec").and_then(parse_data_spec);
        if (!data_spec_result)
        {
            return std::unexpected(data_spec_result.error());
        }
        const auto& data_spec = *data_spec_result;

        const auto& encoding = op_request.at("encoding");
        auto codec_result = get_required<nlohmann::json>(encoding, "codec").and_then(parse_codec);
        if (!codec_result)
        {
            return std::unexpected(codec_result.error());
        }
        const auto codec = *codec_result;

        auto flags_result = parse_flags(encoding.value("flags", nlohmann::json::array()));
        if (!flags_result)
        {
            return std::unexpected(flags_result.error());
        }
        uint64_t flags = *flags_result;
        // If neither endianness flag is set, default to the native endianness of the current architecture.
        if ((flags & (ChunkFlags::LITTLE_ENDIAN | ChunkFlags::BIG_ENDIAN)) == 0)
        {
            if constexpr (std::endian::native == std::endian::little)
            {
                flags |= ChunkFlags::LITTLE_ENDIAN;
            }
            else if constexpr (std::endian::native == std::endian::big)
            {
                flags |= ChunkFlags::BIG_ENDIAN;
            }
        }

        const int zstd_level = encoding.value("zstd_level", ZstdCompressor::DEFAULT_COMPRESSION_LEVEL);

        // 3. Validate input data size
        const size_t expected_bytes =
            std::accumulate(data_spec.shape.begin(), data_spec.shape.end(), size_t{1}, std::multiplies<>()) *
            get_dtype_size(data_spec.dtype);
        if (input_data.size() != expected_bytes)
        {
            return std::unexpected(ExpectedError("Input data size does not match shape and dtype specification."));
        }

        // 4. Dispatch to the correct compression function
        DataCompressor& compressor = context.get_compressor();
        DataCompressor::ChunkResult chunk_result;

        switch (codec)
        {
        case ChunkDataType::RAW:

            {
                auto append_result = writer.append_chunk(codec, data_spec.dtype, flags, data_spec.shape, input_data);
                if (!append_result)
                {
                    return std::unexpected(ExpectedError(append_result.error()));
                }
                nlohmann::json result;
                result["chunks_written"] = 1;
                result["chunk_index"] = *append_result;
                result["original_size"] = input_data.size_bytes();
                result["size"] = input_data.size_bytes();
                result["shape"] = data_spec.shape;
                return result;
            }
        case ChunkDataType::ZSTD_COMPRESSED:
            chunk_result = compressor.compress_zstd(input_data, data_spec.shape, data_spec.dtype, zstd_level);
            flags |= ChunkFlags::ZSTD;
            break;

        // --- 1D Temporal Float Codecs ---
        case ChunkDataType::TEMPORAL_1D_SIMD_F16_XOR_SHUFFLE_AS_F32:
            if (data_spec.dtype != DType::FLOAT32)
            {
                return std::unexpected(
                    ExpectedError("This codec requires FLOAT32 input dtype to produce F16 internal representation."));
            }
            if (input_data.size() % sizeof(float) != 0)
            {
                return std::unexpected(ExpectedError(
                    "Input data size is not a multiple of sizeof(float), indicating misaligned or incomplete data."));
            }
            // TODO: Pass actual prev_element state from CddContext
            chunk_result = compressor.compress_chunk(
                std::span(reinterpret_cast<const float*>(input_data.data()), input_data.size() / sizeof(float)), codec,
                0.0f, zstd_level);
            break;

        case ChunkDataType::TEMPORAL_1D_SIMD_F32_XOR_SHUFFLE:
            if (data_spec.dtype != DType::FLOAT32)
            {
                return std::unexpected(ExpectedError("OKX_OB_SIMD_F32 requires FLOAT32 dtype."));
            }
            if (input_data.size() % sizeof(float) != 0)
            {
                return std::unexpected(ExpectedError(
                    "Input data size is not a multiple of sizeof(float), indicating misaligned or incomplete data."));
            }
            // TODO: Pass actual prev_element state from CddContext
            chunk_result = compressor.compress_chunk(
                std::span(reinterpret_cast<const float*>(input_data.data()), input_data.size() / sizeof(float)), codec,
                0.0f, zstd_level);
            break;

        // --- 2D/3D Float Codecs (Orderbook & Temporal 2D) ---
        case ChunkDataType::OKX_OB_SIMD_F16_AS_F32:
        case ChunkDataType::BINANCE_OB_SIMD_F16_AS_F32:
        case ChunkDataType::GENERIC_OB_SIMD_F16_AS_F32:
        case ChunkDataType::TEMPORAL_2D_SIMD_F16_AS_F32:
        case ChunkDataType::OKX_OB_SIMD_F32:
        case ChunkDataType::BINANCE_OB_SIMD_F32:
        case ChunkDataType::GENERIC_OB_SIMD_F32:
        case ChunkDataType::TEMPORAL_2D_SIMD_F32:
            {
                if (data_spec.dtype != DType::FLOAT32)
                {
                    return std::unexpected(ExpectedError("This codec requires FLOAT32 dtype."));
                }
                if (input_data.size() % sizeof(float) != 0)
                {
                    return std::unexpected(ExpectedError(
                        "Input data size is not a multiple of sizeof(float), indicating misaligned or incomplete data."));
                }
                auto data_span =
                    std::span(reinterpret_cast<const float*>(input_data.data()), input_data.size() / sizeof(float));
                // TODO: Pass actual prev_state from CddContext
                
                size_t prev_state_elements = 0;
                if (data_spec.shape.size() == 2) { // Temporal 2D
                    prev_state_elements = data_spec.shape[1];
                } else if (data_spec.shape.size() == 3) { // Orderbook
                    prev_state_elements = data_spec.shape[1] * data_spec.shape[2];
                } else {
                    return std::unexpected(ExpectedError("Unsupported shape for this codec."));
                }

                const auto zero_state_bytes = context.get_zero_state(prev_state_elements * sizeof(float));
                const std::span<const float> prev_state_span(
                    reinterpret_cast<const float*>(zero_state_bytes.data()),
                    prev_state_elements
                );

                chunk_result = compressor.compress_chunk(data_span, codec, data_spec.shape, prev_state_span, zstd_level);
                break;
            }

        // --- 1D Temporal Integer Codecs ---
        case ChunkDataType::TEMPORAL_1D_SIMD_I64_XOR:
        case ChunkDataType::TEMPORAL_1D_SIMD_I64_DELTA:
            {
                if (data_spec.dtype != DType::INT64)
                {
                    return std::unexpected(ExpectedError("This codec requires INT64 dtype."));
                }
                if (input_data.size() % sizeof(int64_t) != 0)
                {
                    return std::unexpected(ExpectedError("Input data size is not a multiple of sizeof(int64_t), "
                                                         "indicating misaligned or incomplete data."));
                }
                auto data_span =
                    std::span(reinterpret_cast<const int64_t*>(input_data.data()), input_data.size() / sizeof(int64_t));
                // TODO: Pass actual prev_element state from CddContext
                chunk_result = compressor.compress_chunk(data_span, codec, 0, zstd_level);
                break;
            }

        // --- 2D Temporal Integer Codecs ---
        case ChunkDataType::TEMPORAL_2D_SIMD_I64:
            {
                if (data_spec.dtype != DType::INT64)
                {
                    return std::unexpected(ExpectedError("This codec requires INT64 dtype."));
                }
                if (input_data.size() % sizeof(int64_t) != 0)
                {
                    return std::unexpected(ExpectedError("Input data size is not a multiple of sizeof(int64_t), "
                                                         "indicating misaligned or incomplete data."));
                }
                auto data_span =
                    std::span(reinterpret_cast<const int64_t*>(input_data.data()),
                              input_data.size() / sizeof(int64_t));

                // TODO: Pass actual prev_row state from CddContext
                if (data_spec.shape.size() != 2) {
                     return std::unexpected(ExpectedError("Unsupported shape for this codec. Expected 2 dimensions."));
                }
                const size_t prev_state_elements = data_spec.shape[1];
                const auto zero_state_bytes = context.get_zero_state(prev_state_elements * sizeof(int64_t));
                const std::span<const int64_t> prev_state_span(
                    reinterpret_cast<const int64_t*>(zero_state_bytes.data()),
                    prev_state_elements
                );
                chunk_result = compressor.compress_chunk(data_span, codec, data_spec.shape, prev_state_span, zstd_level);
                break;
            }

        default:
            return std::unexpected(ExpectedError("The specified codec is not yet supported for writing."));
        }

        if (!chunk_result)
        {
            return std::unexpected(ExpectedError(chunk_result.error().to_string()));
        }

        // 5. Append the compressed chunk
        auto& chunk = *chunk_result;
        auto append_result =
            writer.append_chunk(chunk->type(), chunk->dtype(), chunk->flags(), chunk->get_shape(), chunk->data());
        if (!append_result)
        {
            return std::unexpected(ExpectedError(append_result.error()));
        }

        nlohmann::json result;
        result["chunks_written"] = 1;
        result["chunk_index"] = *append_result;
        result["original_size"] = input_data.size_bytes();
        result["size"] = chunk->data().size();
        result["shape"] = data_spec.shape;
        result["zstd_level"] = zstd_level;
        return result;
    }

} // namespace cryptodd::ffi
