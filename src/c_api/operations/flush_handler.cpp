#include "../operations/flush_handler.h"
#include "../operations/json_serialization.h"
#include "../../data_io/data_writer.h" // For DataWriter
#include <nlohmann/json.hpp>

namespace cryptodd::ffi {

// The "Adapter"
std::expected<nlohmann::json, ExpectedError> FlushHandler::execute(
    CddContext& context, const nlohmann::json& op_request, std::span<const std::byte>, std::span<std::byte>)
{
    auto request_result = from_json<FlushRequest>(op_request);
    if (!request_result) return std::unexpected(request_result.error());
    
    auto response_result = execute_typed(context, *request_result);
    if (!response_result) return std::unexpected(response_result.error());

    return to_json(*response_result);
}

// The "Business Logic"
std::expected<FlushResponse, ExpectedError> FlushHandler::execute_typed(
    CddContext& context, const FlushRequest& request)
{
    auto writer_opt = context.get_writer();
    if (!writer_opt) return std::unexpected(ExpectedError("Context is not in a writable mode."));
    
    auto result = writer_opt.value().get().flush();
    if (!result) return std::unexpected(ExpectedError(result.error()));

    FlushResponse response;
    response.client_key = request.client_key;
    response.status = "Flush completed.";
    // Metadata will be injected at the C API layer
    return response;
}

} // namespace cryptodd::ffi
