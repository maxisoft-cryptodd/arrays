#pragma once

#include "../cdd_context.h" // For ExpectedError
#include <nlohmann/json_fwd.hpp>
#include <expected>

namespace cryptodd::ffi {

// Forward declare all request/response types so this header remains lightweight.
struct StoreChunkRequest; struct StoreArrayRequest; struct LoadChunksRequest;
struct InspectRequest; struct GetUserMetadataRequest; struct SetUserMetadataRequest;
struct FlushRequest; struct PingRequest;

struct StoreChunkResponse; struct StoreArrayResponse; struct LoadChunksResponse;
struct InspectResponse; struct GetUserMetadataResponse; struct SetUserMetadataResponse;
struct FlushResponse; struct PingResponse;

// Generic deserializer from a JSON object to a strongly-typed request struct.
// It catches parsing/validation exceptions and converts them to ExpectedError.
template<typename T>
std::expected<T, ExpectedError> from_json(const nlohmann::json& j);

// Generic serializer from a strongly-typed response struct to a JSON object.
template<typename T>
nlohmann::json to_json(const T& response);

} // namespace cryptodd::ffi
