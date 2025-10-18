#include "../operations/store_array_handler.h"
#include "../operations/json_serialization.h"
#include "../operations/store_utils.h"
#include "../../file_format/cdd_file_format.h" // For get_dtype_size
#include "../../data_io/data_writer.h" // For DataWriter
#include <numeric> // For std::accumulate
#include <algorithm> // For std::min
#include <variant> // For std::get_if

namespace cryptodd::ffi {

// The "Adapter"
std::expected<nlohmann::json, ExpectedError> StoreArrayHandler::execute(
    CddContext& context, const nlohmann::json& op_request, std::span<const std::byte> input_data, std::span<std::byte>)
{
    auto request_result = from_json<StoreArrayRequest>(op_request);
    if (!request_result) return std::unexpected(request_result.error());
    
    auto response_result = execute_typed(context, *request_result, input_data);
    if (!response_result) return std::unexpected(response_result.error());

    return to_json(*response_result);
}

// The "Business Logic"
std::expected<StoreArrayResponse, ExpectedError> StoreArrayHandler::execute_typed(
    CddContext& context, const StoreArrayRequest& request, std::span<const std::byte> input_data)
{
    auto writer_opt = context.get_writer();
    if (!writer_opt) return std::unexpected(ExpectedError("Context is not in a writable mode."));
    cryptodd::DataWriter& writer = writer_opt.value().get();

    const auto& full_data_spec = request.data_spec;
    const auto& encoding_spec = request.encoding;
    const auto& chunking_strategy = request.chunking_strategy;

    const int64_t rows_per_chunk = std::visit([]<typename T0>(T0&& arg) -> int64_t {
        using T = std::decay_t<T0>;
        if constexpr (std::is_same_v<T, ByCountChunking>) {
            return arg.rows_per_chunk;
        }
        // Should not happen with current variant definition, but good for completeness
        return -1; 
    }, chunking_strategy);

    if (rows_per_chunk <= 0) return std::unexpected(ExpectedError("rows_per_chunk must be positive."));
    if (full_data_spec.shape.empty()) return std::unexpected(ExpectedError("Cannot use ByCount strategy on a 0-dimensional array."));

    const int64_t total_rows = full_data_spec.shape[0];
    const size_t dtype_size = get_dtype_size(full_data_spec.dtype);
    size_t row_size_bytes = dtype_size;
    if (full_data_spec.shape.size() > 1) {
        row_size_bytes = std::accumulate(full_data_spec.shape.begin() + 1, full_data_spec.shape.end(), size_t{1}, std::multiplies<>()) * dtype_size;
    }
    
    StoreArrayResponse response;
    response.client_key = request.client_key;
    response.chunks_written = 0;
    response.total_original_bytes = 0;
    response.total_compressed_bytes = 0;
    response.chunk_details = std::vector<ChunkWriteDetails>();

    for (int64_t start_row = 0; start_row < total_rows; start_row += rows_per_chunk) {
        const int64_t current_rows = std::min(rows_per_chunk, total_rows - start_row);
        
        DataSpec chunk_spec = std::cref(full_data_spec);
        chunk_spec.shape[0] = current_rows;

        const size_t offset_bytes = start_row * row_size_bytes;
        const size_t length_bytes = current_rows * row_size_bytes;
        std::span<const std::byte> chunk_data_span(input_data.data() + offset_bytes, length_bytes);
        
        auto result = StoreUtils::compress_and_write_chunk(context, writer, chunk_spec, encoding_spec, chunk_data_span);
        if (!result) return std::unexpected(result.error());

        response.chunks_written++;
        response.total_original_bytes += result->original_size;
        response.total_compressed_bytes += result->compressed_size;
        response.chunk_details.push_back(*result);
    }
    
    return response;
}

} // namespace cryptodd::ffi
