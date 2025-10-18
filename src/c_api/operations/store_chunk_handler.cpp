#include "../operations/store_chunk_handler.h"
#include "../operations/json_serialization.h"
#include "../operations/store_utils.h"
#include "../../codecs/zstd_compressor.h" // For ZstdCompressor::DEFAULT_COMPRESSION_LEVEL
#include "../../file_format/cdd_file_format.h" // For get_dtype_size
#include "../../data_io/data_writer.h" // For DataWriter
#include <numeric> // For std::accumulate

namespace cryptodd::ffi {

// The "Adapter"
std::expected<nlohmann::json, ExpectedError> StoreChunkHandler::execute(
    CddContext& context, const nlohmann::json& op_request, std::span<const std::byte> input_data, std::span<std::byte>)
{
    auto request_result = from_json<StoreChunkRequest>(op_request);
    if (!request_result) return std::unexpected(request_result.error());
    
    auto response_result = execute_typed(context, *request_result, input_data);
    if (!response_result) return std::unexpected(response_result.error());

    return to_json(*response_result);
}

// The "Business Logic"
std::expected<StoreChunkResponse, ExpectedError> StoreChunkHandler::execute_typed(
    CddContext& context, const StoreChunkRequest& request, std::span<const std::byte> input_data)
{
    auto writer_opt = context.get_writer();
    if (!writer_opt) {
        return std::unexpected(ExpectedError("Context is not in a writable mode."));
    }
    cryptodd::DataWriter& writer = writer_opt.value().get();

    const size_t expected_bytes =
        std::accumulate(request.data_spec.shape.begin(), request.data_spec.shape.end(), size_t{1}, std::multiplies<>()) *
        get_dtype_size(request.data_spec.dtype);
    if (input_data.size() != expected_bytes) {
        return std::unexpected(ExpectedError("Input data size does not match shape and dtype specification."));
    }

    auto result = StoreUtils::compress_and_write_chunk(context, writer, request.data_spec, request.encoding, input_data);
    if (!result) return std::unexpected(result.error());

    StoreChunkResponse response;
    response.client_key = request.client_key;
    response.details = *result;
    response.shape = request.data_spec.shape;
    response.zstd_level = request.encoding.zstd_level.value_or(cryptodd::ZstdCompressor::DEFAULT_COMPRESSION_LEVEL);
    // Metadata will be injected at the C API layer

    return response;
}

} // namespace cryptodd::ffi
