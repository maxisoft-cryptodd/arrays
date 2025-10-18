#define MAGIC_ENUM_ENABLE_HASH 1
#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>

#include "../operations/json_serialization.h"
#include "../operations/operation_types.h"
#include "../base64.h"

namespace cryptodd::ffi {

// ========================================================================
// Serialization Helpers
// ========================================================================

// Helper to safely get a required field from a JSON object
template<typename T>
static T get_required(const nlohmann::json& j, const char* key) {
    if (!j.contains(key)) {
        throw nlohmann::json::out_of_range::create(403, std::string("missing required key: '") + key + "'", &j);
    }
    try {
        return j.at(key).get<T>();
    } catch (const nlohmann::json::exception& e) {
        throw nlohmann::json::type_error::create(302, std::string("failed to parse key '") + key + "': " + e.what(), &j);
    }
}

// Enum from/to JSON helpers using magic_enum
template<typename E>
static void enum_from_json(const nlohmann::json& j, E& e) {
    const auto str = j.get<std::string>();
    if (auto val = magic_enum::enum_cast<E>(str); val.has_value()) {
        e = val.value();
    } else {
        throw nlohmann::json::type_error::create(302, "invalid enum value '" + str + "' for type " + std::string(magic_enum::enum_type_name<E>()), &j);
    }
}

template<typename E>
static void enum_to_json(nlohmann::json& j, const E& e) {
    j = magic_enum::enum_name(e);
}

// Base class from/to JSON helpers for client_key
template<typename T>
static void from_json_base(const nlohmann::json& j, T& base) {
    base.client_key = j.value<std::optional<std::string>>("client_key", std::nullopt);
}
template<typename T>
static void to_json_base(nlohmann::json& j, const T& base) {
    if (base.client_key) {
        j["client_key"] = *base.client_key;
    }
}

// ========================================================================
// ADL Implementations for nlohmann::json
// ========================================================================

// We need to be in the cryptodd::ffi namespace to define these ADL functions.
void to_json(nlohmann::json& j, const DataSpec& spec) { j = {{"dtype", magic_enum::enum_name(spec.dtype)}, {"shape", spec.shape}}; }
void from_json(const nlohmann::json& j, DataSpec& spec) { enum_from_json(get_required<nlohmann::json>(j, "dtype"), spec.dtype); spec.shape = get_required<std::vector<int64_t>>(j, "shape"); }

void to_json(nlohmann::json& j, const EncodingSpec& spec) { j = {{"codec", magic_enum::enum_name(spec.codec)}, {"flags", spec.flags}, {"zstd_level", spec.zstd_level}}; }
void from_json(const nlohmann::json& j, EncodingSpec& spec) { enum_from_json(get_required<nlohmann::json>(j, "codec"), spec.codec); spec.flags = j.value("flags", std::vector<std::string>{}); spec.zstd_level = j.value<std::optional<int>>("zstd_level", std::nullopt); }

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ByCountChunking, rows_per_chunk)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(OperationMetadata, backend_type, mode, duration_us)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ChunkWriteDetails, chunk_index, original_size, compressed_size, compression_ratio)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ChunkSummary, index, shape, dtype, codec, encoded_size_bytes, decoded_size_bytes)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FileHeaderInfo, version, index_block_offset, index_block_size, user_metadata_base64)

// --- StoreChunk ---
void from_json(const nlohmann::json& j, StoreChunkRequest& req) { from_json_base(j, req); req.data_spec = get_required<DataSpec>(j, "data_spec"); req.encoding = get_required<EncodingSpec>(j, "encoding"); }
void to_json(nlohmann::json& j, const StoreChunkResponse& res) { to_json_base(j, res); j["details"] = res.details; j["shape"] = res.shape; j["zstd_level"] = res.zstd_level; j["metadata"] = res.metadata; }

// --- StoreArray ---
void from_json(const nlohmann::json& j, ChunkingStrategy& s); // Implemented below
void to_json(nlohmann::json& j, const ChunkingStrategy& s);   // Implemented below
void from_json(const nlohmann::json& j, StoreArrayRequest& req) { from_json_base(j, req); req.data_spec = get_required<DataSpec>(j, "data_spec"); req.encoding = get_required<EncodingSpec>(j, "encoding"); req.chunking_strategy = get_required<ChunkingStrategy>(j, "chunking_strategy"); }
void to_json(nlohmann::json& j, const StoreArrayResponse& res) { to_json_base(j, res); j["chunks_written"] = res.chunks_written; j["total_original_bytes"] = res.total_original_bytes; j["total_compressed_bytes"] = res.total_compressed_bytes; j["chunk_details"] = res.chunk_details; j["metadata"] = res.metadata; }

// --- LoadChunks ---
void from_json(const nlohmann::json& j, ChunkSelection& s); // Implemented below
void to_json(nlohmann::json& j, const ChunkSelection& s);   // Implemented below
void from_json(const nlohmann::json& j, LoadChunksRequest& req) { from_json_base(j, req); req.selection = get_required<ChunkSelection>(j, "selection"); }
void to_json(nlohmann::json& j, const LoadChunksResponse& res) { to_json_base(j, res); j["bytes_written_to_output"] = res.bytes_written_to_output; if (res.final_shape) j["final_shape"] = *res.final_shape; j["metadata"] = res.metadata; }

// --- Inspect ---
void from_json(const nlohmann::json& j, InspectRequest& req) { from_json_base(j, req); req.calculate_checksums = j.value("calculate_checksums", false); }
void to_json(nlohmann::json& j, const InspectResponse& res) { to_json_base(j, res); j["file_header"] = res.file_header; j["total_chunks"] = res.total_chunks; j["chunk_summaries"] = res.chunk_summaries; j["metadata"] = res.metadata; }

// --- Metadata ---
void from_json(const nlohmann::json& j, GetUserMetadataRequest& req) { from_json_base(j, req); }
void to_json(nlohmann::json& j, const GetUserMetadataResponse& res) { to_json_base(j, res); j["user_metadata_base64"] = res.user_metadata_base64; j["metadata"] = res.metadata; }
void from_json(const nlohmann::json& j, SetUserMetadataRequest& req) { from_json_base(j, req); req.user_metadata_base64 = get_required<std::string>(j, "user_metadata_base64"); }
void to_json(nlohmann::json& j, const SetUserMetadataResponse& res) { to_json_base(j, res); j["status"] = res.status; j["metadata"] = res.metadata; }

// --- Flush ---
void from_json(const nlohmann::json& j, FlushRequest& req) { from_json_base(j, req); }
void to_json(nlohmann::json& j, const FlushResponse& res) { to_json_base(j, res); j["status"] = res.status; j["metadata"] = res.metadata; }

// --- Ping ---
void from_json(const nlohmann::json& j, PingRequest& req) { from_json_base(j, req); }
void to_json(nlohmann::json& j, const PingResponse& res) { to_json_base(j, res); j["message"] = res.message; j["metadata"] = res.metadata; }

// --- Custom logic for std::variant types ---
void from_json(const nlohmann::json& j, ChunkingStrategy& s) {
    const auto strategy_type = get_required<std::string>(j, "strategy");
    if (strategy_type == "ByCount") { s = j.get<ByCountChunking>(); }
    else { throw nlohmann::json::type_error::create(302, "unsupported chunking strategy: " + strategy_type, &j); }
}
void to_json(nlohmann::json& j, const ChunkingStrategy& s) {
    std::visit([&](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, ByCountChunking>) {
            j = nlohmann::json(arg);
            j["strategy"] = "ByCount";
        }
    }, s);
}

void from_json(const nlohmann::json& j, ChunkSelection& s) {
    const auto type = get_required<std::string>(j, "type");
    if (type == "All") { s = AllSelection{}; }
    else if (type == "Indices") { s = IndicesSelection{ get_required<std::vector<size_t>>(j, "indices") }; }
    else if (type == "Range") { s = RangeSelection{ get_required<size_t>(j, "start_index"), get_required<size_t>(j, "count") }; }
    else { throw nlohmann::json::type_error::create(302, "unknown selection type: " + type, &j); }
}
void to_json(nlohmann::json& j, const ChunkSelection& s) {
    std::visit([&](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, AllSelection>) {
            j["type"] = "All";
        } else if constexpr (std::is_same_v<T, IndicesSelection>) {
            j["type"] = "Indices";
            j["indices"] = arg.indices;
        } else if constexpr (std::is_same_v<T, RangeSelection>) {
            j["type"] = "Range";
            j["start_index"] = arg.start_index;
            j["count"] = arg.count;
        }
    }, s);
}

// ========================================================================
// Generic Template Instantiations
// ========================================================================

template<typename T>
std::expected<T, ExpectedError> from_json(const nlohmann::json& j) {
    try {
        T value;
        from_json(j, value);
        return value;
    } catch (const nlohmann::json::exception& e) {
        return std::unexpected(ExpectedError(std::string("JSON request validation error: ") + e.what()));
    }
}
#define INSTANTIATE_FROM_JSON(T) template std::expected<T, ExpectedError> from_json<T>(const nlohmann::json&);
INSTANTIATE_FROM_JSON(StoreChunkRequest) INSTANTIATE_FROM_JSON(StoreArrayRequest)
INSTANTIATE_FROM_JSON(LoadChunksRequest) INSTANTIATE_FROM_JSON(InspectRequest)
INSTANTIATE_FROM_JSON(GetUserMetadataRequest) INSTANTIATE_FROM_JSON(SetUserMetadataRequest)
INSTANTIATE_FROM_JSON(FlushRequest) INSTANTIATE_FROM_JSON(PingRequest)
#undef INSTANTIATE_FROM_JSON

template<typename T>
nlohmann::json to_json(const T& response) {
    nlohmann::json j;
    to_json(j, response);
    return j;
}
#define INSTANTIATE_TO_JSON(T) template nlohmann::json to_json<T>(const T&);
INSTANTIATE_TO_JSON(StoreChunkResponse) INSTANTIATE_TO_JSON(StoreArrayResponse)
INSTANTIATE_TO_JSON(LoadChunksResponse) INSTANTIATE_TO_JSON(InspectResponse)
INSTANTIATE_TO_JSON(GetUserMetadataResponse) INSTANTIATE_TO_JSON(SetUserMetadataResponse)
INSTANTIATE_TO_JSON(FlushResponse) INSTANTIATE_TO_JSON(PingResponse)
#undef INSTANTIATE_TO_JSON

} // namespace cryptodd::ffi
