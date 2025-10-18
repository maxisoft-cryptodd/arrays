#include "../operations/ping_handler.h"
#include "../operations/json_serialization.h"
#include <nlohmann/json.hpp>

namespace cryptodd::ffi {

// The "Adapter"
std::expected<nlohmann::json, ExpectedError> PingHandler::execute(
    CddContext& context, const nlohmann::json& op_request, std::span<const std::byte>, std::span<std::byte>)
{
    auto request_result = from_json<PingRequest>(op_request);
    if (!request_result) return std::unexpected(request_result.error());
    
    auto response_result = execute_typed(context, *request_result);
    if (!response_result) return std::unexpected(response_result.error());

    return to_json(*response_result);
}

// The "Business Logic"
std::expected<PingResponse, ExpectedError> PingHandler::execute_typed(
    CddContext& context, const PingRequest& request)
{
    PingResponse response;
    response.client_key = request.client_key;
    response.message = "Pong";
    // Metadata will be injected at the C API layer
    return response;
}

} // namespace cryptodd::ffi
