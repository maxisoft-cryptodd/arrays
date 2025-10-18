#include "store_array_handler.h"
#include <numeric>
#include "../c_api_utils.h"

namespace cryptodd::ffi {

using namespace utils;

std::expected<nlohmann::json, ExpectedError> StoreArrayHandler::execute(
    CddContext& context,
    const nlohmann::json& op_request,
    std::span<const std::byte> input_data,
    std::span<std::byte> /*output_data*/)
{
    auto writer_opt = context.get_writer();
    if (!writer_opt) return std::unexpected(ExpectedError("Context is not in a writable mode."));
    DataWriter& writer = writer_opt.value().get();

    auto data_spec_result = get_required<nlohmann::json>(op_request, "data_spec").and_then(parse_data_spec);
    if (!data_spec_result) return std::unexpected(data_spec_result.error());
    const auto& full_data_spec = *data_spec_result;

    const auto& encoding_json = op_request.at("encoding");

    // --- Parse Chunking Strategy ---
    const auto& chunking_json = op_request.at("chunking_strategy");
    const auto strategy = chunking_json.at("strategy").get<std::string>();
    
    if (strategy != "ByCount") {
        return std::unexpected(ExpectedError("Unsupported chunking strategy: " + strategy));
    }

    const int64_t rows_per_chunk = chunking_json.at("rows_per_chunk").get<int64_t>();
    if (rows_per_chunk <= 0) return std::unexpected(ExpectedError("rows_per_chunk must be positive."));
    if (full_data_spec.shape.empty()) return std::unexpected(ExpectedError("Cannot use ByCount strategy on a 0-dimensional array."));

    const int64_t total_rows = full_data_spec.shape[0];
    const size_t dtype_size = get_dtype_size(full_data_spec.dtype);
    size_t row_size_bytes = dtype_size;
    if (full_data_spec.shape.size() > 1) {
        row_size_bytes = std::accumulate(full_data_spec.shape.begin() + 1, full_data_spec.shape.end(), size_t{1}, std::multiplies<>()) * dtype_size;
    }
    
    nlohmann::json response;
    response["chunks_written"] = 0;
    response["chunk_details"] = nlohmann::json::array();
    int64_t total_original_bytes = 0;
    int64_t total_compressed_bytes = 0;

    for (int64_t start_row = 0; start_row < total_rows; start_row += rows_per_chunk) {
        const int64_t current_rows = std::min(rows_per_chunk, total_rows - start_row);
        
        // Prepare spec for this specific chunk
        utils::DataSpec chunk_spec = full_data_spec;
        chunk_spec.shape[0] = current_rows;

        // Slice the input data
        const size_t offset_bytes = start_row * row_size_bytes;
        const size_t length_bytes = current_rows * row_size_bytes;
        std::span<const std::byte> chunk_data_span(input_data.data() + offset_bytes, length_bytes);
        
        // Process this chunk using the shared logic
        auto result = compress_and_write_chunk(context, writer, chunk_spec, encoding_json, chunk_data_span);
        if (!result) return result;

        // Aggregate results
        response["chunks_written"] = response["chunks_written"].get<int>() + 1;
        total_original_bytes += (*result)["original_size"].get<int64_t>();
        total_compressed_bytes += (*result)["size"].get<int64_t>();
        response["chunk_details"].push_back(*result);
    }
    
    response["original_size"] = total_original_bytes;
    response["size"] = total_compressed_bytes;
    return response;
}

} // namespace cryptodd::ffi
