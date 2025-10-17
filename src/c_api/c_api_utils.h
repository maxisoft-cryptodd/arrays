#pragma once

#include "../c_api/cdd_context.h"
#include "../file_format/cdd_file_format.h"
#include <nlohmann/json.hpp>
#include <expected>
#include <string>
#include <vector>

namespace cryptodd::ffi::utils {

// Helper to safely get a required value from a JSON object
template<typename T>
std::expected<T, ExpectedError> get_required(const nlohmann::json& j, const char* key) {
    if (!j.contains(key)) {
        return std::unexpected(ExpectedError(std::string("Missing required key: '") + key + "'"));
    }
    try {
        return j.at(key).get<T>();
    } catch (const nlohmann::json::exception& e) {
        return std::unexpected(ExpectedError(std::string("Failed to parse key '") + key + "': " + e.what()));
    }
}

// Parses a string like "INT64" into a DType enum.
std::expected<DType, ExpectedError> parse_dtype(const nlohmann::json& j);

// Parses a string like "TEMPORAL_1D_SIMD_I64_DELTA" into a ChunkDataType enum.
std::expected<ChunkDataType, ExpectedError> parse_codec(const nlohmann::json& j);

// Parses an array of strings like ["LITTLE_ENDIAN"] into a bitmask of ChunkFlags.
std::expected<uint64_t, ExpectedError> parse_flags(const nlohmann::json& j);

struct DataSpec {
    DType dtype;
    std::vector<int64_t> shape;
};

// Parses the "data_spec" block from a request.
std::expected<DataSpec, ExpectedError> parse_data_spec(const nlohmann::json& j);

} // namespace cryptodd::ffi::utils
