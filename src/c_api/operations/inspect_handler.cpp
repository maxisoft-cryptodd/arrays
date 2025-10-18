#define MAGIC_ENUM_ENABLE_HASH 1
#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>

#include "../../codecs/zstd_compressor.h" // For decompression
#include "../../data_io/data_reader.h" // For DataReader
#include "../../file_format/cdd_file_format.h" // For FileHeader
#include "../cdd_context.h"
#include "../operations/inspect_handler.h"
#include "base64.h" // For base64::encode
#include "json_serialization.h"

namespace cryptodd::ffi {

// The "Adapter"
std::expected<nlohmann::json, ExpectedError> InspectHandler::execute(
    CddContext& context, const nlohmann::json& op_request, std::span<const std::byte>, std::span<std::byte>)
{
    auto request_result = from_json<InspectRequest>(op_request);
    if (!request_result) return std::unexpected(request_result.error());
    
    auto response_result = execute_typed(context, *request_result);
    if (!response_result) return std::unexpected(response_result.error());

    // Propagate client_key from request to response
    response_result->client_key = request_result->client_key;
    return to_json(*response_result);
}

// The "Business Logic"
std::expected<InspectResponse, ExpectedError> InspectHandler::execute_typed(
    CddContext& context, const InspectRequest& request)
{
    auto reader_opt = context.get_reader();
    if (!reader_opt) return std::unexpected(ExpectedError("Context is not in a readable mode."));
    cryptodd::DataReader& reader = reader_opt.value().get();

    InspectResponse response;
    response.client_key = request.client_key;

    // Populate FileHeaderInfo
    const auto& header = reader.get_file_header();

    // FIX: Decompress user metadata before encoding to base64
    std::string user_meta_b64;
    const auto& compressed_meta = header.user_metadata();
    if (!compressed_meta.empty()) {
        ZstdCompressor compressor;
        auto decompressed_res = compressor.decompress(compressed_meta);
        if (!decompressed_res) {
            return std::unexpected(ExpectedError("Failed to decompress user metadata for Inspect operation: " + decompressed_res.error()));
        }
        user_meta_b64 = base64::encode(*decompressed_res);
    }

    response.file_header = {
        .version = header.version(), .index_block_offset = reader.get_index_block_offset(),
        .index_block_size = reader.get_index_block_size(),
        .user_metadata_base64 = std::move(user_meta_b64)
    };

    response.total_chunks = reader.num_chunks();
    response.chunk_summaries.reserve(reader.num_chunks());

    for (size_t i = 0; i < reader.num_chunks(); ++i) {
        auto chunk_result = reader.get_chunk(i);
        // NOTE: get_chunk already handles decompression for the chunk *body*,
        // which is why this loop is correct. We only needed to fix the header metadata.
        if (!chunk_result) return std::unexpected(ExpectedError(chunk_result.error()));
        
        response.chunk_summaries.push_back(ChunkSummary {
            .index = i,
            .shape = std::vector<int64_t>(chunk_result->get_shape().begin(), chunk_result->get_shape().end()),
            .dtype = chunk_result->dtype(),
            .codec = chunk_result->type(),
            .encoded_size_bytes = chunk_result->data().size(),
            .decoded_size_bytes = chunk_result->expected_size()
        });
    }
    return response;
}

} // namespace cryptodd::ffi
