#include "../operations/metadata_handler.h"
#include "../operations/json_serialization.h"
#include "../base64.h" // For base64::encode and decode
#include "../../data_io/data_reader.h" // For DataReader
#include "../../data_io/data_writer.h" // For DataWriter
#include "../../file_format/cdd_file_format.h" // For FileHeader
#include <nlohmann/json.hpp>
#include <algorithm> // For std::transform

namespace cryptodd::ffi {

// The "Adapter" for GetUserMetadataHandler
std::expected<nlohmann::json, ExpectedError> GetUserMetadataHandler::execute(
    CddContext& context, const nlohmann::json& op_request, std::span<const std::byte>, std::span<std::byte>)
{
    auto request_result = from_json<GetUserMetadataRequest>(op_request);
    if (!request_result) return std::unexpected(request_result.error());
    
    auto response_result = execute_typed(context, *request_result);
    if (!response_result) return std::unexpected(response_result.error());

    return to_json(*response_result);
}

// The "Business Logic" for GetUserMetadataHandler
std::expected<GetUserMetadataResponse, ExpectedError> GetUserMetadataHandler::execute_typed(
    CddContext& context, const GetUserMetadataRequest& request)
{
    auto reader_opt = context.get_reader();
    if (!reader_opt) return std::unexpected(ExpectedError("Context is not in a readable mode."));
    const auto& header = reader_opt.value().get().get_file_header();
    
    GetUserMetadataResponse response;
    response.client_key = request.client_key;
    response.user_metadata_base64 = base64::encode(header.user_metadata());
    // Metadata will be injected at the C API layer
    return response;
}

// The "Adapter" for SetUserMetadataHandler
std::expected<nlohmann::json, ExpectedError> SetUserMetadataHandler::execute(
    CddContext& context, const nlohmann::json& op_request, std::span<const std::byte>, std::span<std::byte>)
{
    auto request_result = from_json<SetUserMetadataRequest>(op_request);
    if (!request_result) return std::unexpected(request_result.error());
    
    auto response_result = execute_typed(context, *request_result);
    if (!response_result) return std::unexpected(response_result.error());

    return to_json(*response_result);
}

// The "Business Logic" for SetUserMetadataHandler
std::expected<SetUserMetadataResponse, ExpectedError> SetUserMetadataHandler::execute_typed(
    CddContext& context, const SetUserMetadataRequest& request)
{
    auto writer_opt = context.get_writer();
    if (!writer_opt) return std::unexpected(ExpectedError("Context is not in a writable mode."));

    memory::vector<std::byte> metadata_bytes_result;
    try
    {
        metadata_bytes_result = base64::decode(request.user_metadata_base64);
    }
    catch (std::exception& e)
    {
        return std::unexpected(ExpectedError("Failed to decode base64 metadata."));
    }

    auto result = writer_opt.value().get().set_user_metadata(metadata_bytes_result);
    if (!result) return std::unexpected(ExpectedError(result.error()));
    
    SetUserMetadataResponse response;
    response.client_key = request.client_key;
    response.status = "Metadata updated.";
    // Metadata will be injected at the C API layer
    return response;
}

} // namespace cryptodd::ffi
