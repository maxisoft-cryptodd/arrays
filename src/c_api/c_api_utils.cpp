#include "c_api_utils.h"
#include <unordered_map>

namespace cryptodd::ffi::utils {

// Using static maps for efficient string-to-enum lookups
static const std::unordered_map<std::string, DType> dtype_map = {
    {"FLOAT16", DType::FLOAT16},
    {"FLOAT32", DType::FLOAT32},
    {"FLOAT64", DType::FLOAT64},
    {"INT8", DType::INT8},
    {"UINT8", DType::UINT8},
    {"INT16", DType::INT16},
    {"UINT16", DType::UINT16},
    {"INT32", DType::INT32},
    {"UINT32", DType::UINT32},
    {"INT64", DType::INT64},
    {"UINT64", DType::UINT64},
    {"BFLOAT16", DType::BFLOAT16}
};

static const std::unordered_map<std::string, ChunkDataType> codec_map = {
    {"RAW", ChunkDataType::RAW},
    {"ZSTD_COMPRESSED", ChunkDataType::ZSTD_COMPRESSED},
    {"OKX_OB_SIMD_F16_AS_F32", ChunkDataType::OKX_OB_SIMD_F16_AS_F32},
    {"OKX_OB_SIMD_F32", ChunkDataType::OKX_OB_SIMD_F32},
    {"BINANCE_OB_SIMD_F16_AS_F32", ChunkDataType::BINANCE_OB_SIMD_F16_AS_F32},
    {"BINANCE_OB_SIMD_F32", ChunkDataType::BINANCE_OB_SIMD_F32},
    {"GENERIC_OB_SIMD_F16_AS_F32", ChunkDataType::GENERIC_OB_SIMD_F16_AS_F32},
    {"GENERIC_OB_SIMD_F32", ChunkDataType::GENERIC_OB_SIMD_F32},
    {"TEMPORAL_1D_SIMD_F16_XOR_SHUFFLE_AS_F32", ChunkDataType::TEMPORAL_1D_SIMD_F16_XOR_SHUFFLE_AS_F32},
    {"TEMPORAL_1D_SIMD_F32_XOR_SHUFFLE", ChunkDataType::TEMPORAL_1D_SIMD_F32_XOR_SHUFFLE},
    {"TEMPORAL_1D_SIMD_I64_DELTA", ChunkDataType::TEMPORAL_1D_SIMD_I64_DELTA},
    {"TEMPORAL_1D_SIMD_I64_XOR", ChunkDataType::TEMPORAL_1D_SIMD_I64_XOR},
    {"TEMPORAL_2D_SIMD_F16_AS_F32", ChunkDataType::TEMPORAL_2D_SIMD_F16_AS_F32},
    {"TEMPORAL_2D_SIMD_F32", ChunkDataType::TEMPORAL_2D_SIMD_F32},
    {"TEMPORAL_2D_SIMD_I64", ChunkDataType::TEMPORAL_2D_SIMD_I64}
};

static const std::unordered_map<std::string, ChunkFlags> flags_map = {
    {"LZ4", ChunkFlags::LZ4},
    {"ZSTD", ChunkFlags::ZSTD},
    {"LITTLE_ENDIAN", ChunkFlags::LITTLE_ENDIAN},
    {"BIG_ENDIAN", ChunkFlags::BIG_ENDIAN},
    {"DOWN_CAST_8", ChunkFlags::DOWN_CAST_8},
    {"DOWN_CAST_16", ChunkFlags::DOWN_CAST_16},
    {"DOWN_CAST_32", ChunkFlags::DOWN_CAST_32},
    {"DOWN_CAST_64", ChunkFlags::DOWN_CAST_64},
    {"DOWN_CAST_128", ChunkFlags::DOWN_CAST_128}
};


std::expected<DType, ExpectedError> parse_dtype(const nlohmann::json& j) {
    const auto str = j.get<std::string>();
    if (auto it = dtype_map.find(str); it != dtype_map.end()) {
        return it->second;
    }
    return std::unexpected(ExpectedError("Unknown dtype: " + str));
}

std::expected<ChunkDataType, ExpectedError> parse_codec(const nlohmann::json& j) {
    const auto str = j.get<std::string>();
    if (auto it = codec_map.find(str); it != codec_map.end()) {
        return it->second;
    }
    return std::unexpected(ExpectedError("Unknown codec: " + str));
}

std::expected<uint64_t, ExpectedError> parse_flags(const nlohmann::json& j) {
    if (!j.is_array()) return 0; // No flags is valid
    uint64_t mask = 0;
    for (const auto& flag_str_json : j) {
        const auto flag_str = flag_str_json.get<std::string>();
        if (auto it = flags_map.find(flag_str); it != flags_map.end()) {
            mask |= it->second;
        } else {
            return std::unexpected(ExpectedError("Unknown flag: " + flag_str));
        }
    }
    return mask;
}

std::expected<DataSpec, ExpectedError> parse_data_spec(const nlohmann::json& j) {
    if (!j.is_object()) return std::unexpected(ExpectedError("'data_spec' must be an object."));
    
    auto dtype_result = get_required<nlohmann::json>(j, "dtype").and_then(parse_dtype);
    if (!dtype_result) return std::unexpected(dtype_result.error());

    auto shape_result = get_required<std::vector<int64_t>>(j, "shape");
    if (!shape_result) return std::unexpected(shape_result.error());

    return DataSpec{ *dtype_result, std::move(*shape_result) };
}

} // namespace cryptodd::ffi::utils
