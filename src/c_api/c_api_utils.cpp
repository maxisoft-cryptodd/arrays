#include "c_api_utils.h"
#define MAGIC_ENUM_ENABLE_HASH 1
#include <magic_enum/magic_enum.hpp>

namespace cryptodd::ffi::utils {

std::expected<DType, ExpectedError> parse_dtype(const nlohmann::json& j) {
    const auto str = j.get<std::string>();
    if (auto dtype = magic_enum::enum_cast<DType>(str); dtype.has_value()) {
        return dtype.value();
    }
    return std::unexpected(ExpectedError("Unknown dtype: " + str));
}

std::expected<ChunkDataType, ExpectedError> parse_codec(const nlohmann::json& j) {
    const auto str = j.get<std::string>();
    if (auto codec = magic_enum::enum_cast<ChunkDataType>(str); codec.has_value()) {
        return codec.value();
    }
    return std::unexpected(ExpectedError("Unknown codec: " + str));
}

std::expected<uint64_t, ExpectedError> parse_flags(const nlohmann::json& j) {
    if (!j.is_array()) return 0; // No flags is valid
    uint64_t mask = 0;
    for (const auto& flag_str_json : j) {
        const auto flag_str = flag_str_json.get<std::string>();
        if (auto flag = magic_enum::enum_cast<ChunkFlags>(flag_str); flag.has_value()) {
            mask |= static_cast<uint64_t>(flag.value());
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
