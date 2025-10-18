#include "metadata_handler.h"
#include <algorithm>
#include "../base64.h"

namespace cryptodd::ffi {
// GetUserMetadataHandler
std::expected<nlohmann::json, ExpectedError> GetUserMetadataHandler::execute(CddContext& context,
    const nlohmann::json&, std::span<const std::byte>, std::span<std::byte>)
{
    auto reader_opt = context.get_reader();
    if (!reader_opt) return std::unexpected(ExpectedError("Context is not in a readable mode."));
    const auto& header = reader_opt.value().get().get_file_header();
    
    nlohmann::json result;
    result["user_metadata_base64"] = base64::encode(header.user_metadata());
    return result;
}

// SetUserMetadataHandler
std::expected<nlohmann::json, ExpectedError> SetUserMetadataHandler::execute(CddContext& context,
    const nlohmann::json& op_request, std::span<const std::byte>, std::span<std::byte>)
{
    auto writer_opt = context.get_writer();
    if (!writer_opt) return std::unexpected(ExpectedError("Context is not in a writable mode."));
    
    // NOTE: For now, we assume decode is not needed and pass raw string bytes
    auto metadata_str = op_request.at("user_metadata_base64").get<std::string>();
    std::vector<std::byte> metadata_bytes(metadata_str.size());
    std::transform(metadata_str.begin(), metadata_str.end(), metadata_bytes.begin(), [](char c){ return std::byte(c); });
    
    auto result = writer_opt.value().get().set_user_metadata(metadata_bytes);
    if (!result) return std::unexpected(ExpectedError(result.error()));
    
    return nlohmann::json{{"status", "Metadata updated."}};
}
}
