#include "../operations/load_chunks_handler.h"
#include "../cdd_context.h"
#include "../operations/json_serialization.h"
#include "../../data_io/data_reader.h"
#include "../../data_io/data_extractor.h"
#include "../../file_format/cdd_file_format.h"

#include <numeric>
#include <variant>
#include <cstring>
#include <algorithm> // For std::all_of

namespace cryptodd::ffi {

std::expected<nlohmann::json, ExpectedError> LoadChunksHandler::execute(
    CddContext& context, const nlohmann::json& op_request, std::span<const std::byte>, std::span<std::byte> output_data)
{
    auto request_result = from_json<LoadChunksRequest>(op_request);
    if (!request_result) return std::unexpected(request_result.error());
    
    auto response_result = execute_typed(context, *request_result, output_data);
    if (!response_result) return std::unexpected(response_result.error());
    
    response_result->client_key = request_result->client_key;
    return to_json(*response_result);
}

std::expected<LoadChunksResponse, ExpectedError> LoadChunksHandler::execute_typed(
    CddContext& context, const LoadChunksRequest& request, std::span<std::byte> output_data)
{
    auto reader_opt = context.get_reader();
    if (!reader_opt) return std::unexpected(ExpectedError("Context is not in a readable mode."));
    cryptodd::DataReader& reader = reader_opt.value().get();
    cryptodd::DataExtractor& extractor = context.get_extractor();

    std::vector<size_t> indices_to_load;
    std::visit([&]<typename T0>(T0&& arg) {
        using T = std::decay_t<T0>;
        if constexpr (std::is_same_v<T, AllSelection>) {
            indices_to_load.resize(reader.num_chunks());
            std::iota(indices_to_load.begin(), indices_to_load.end(), 0);
        } else if constexpr (std::is_same_v<T, IndicesSelection>) {
            indices_to_load = arg.indices;
        } else if constexpr (std::is_same_v<T, RangeSelection>) {
            for (size_t i = 0; i < arg.count && arg.start_index + i < reader.num_chunks(); ++i) {
                indices_to_load.push_back(arg.start_index + i);
            }
        }
    }, request.selection);

    LoadChunksResponse response;
    if (indices_to_load.empty()) {
        response.bytes_written_to_output = 0;
        return response;
    }

    size_t total_decoded_size = 0;
    std::vector<std::unique_ptr<cryptodd::Chunk>> chunks;
    chunks.reserve(indices_to_load.size());

    std::optional<cryptodd::DType> first_dtype;
    std::optional<std::vector<int64_t>> first_shape_tail; // Shape excluding the first dimension
    bool compatible_shapes = true;
    int64_t sum_first_dim = 0;

    for (const auto index : indices_to_load) {
        if (index >= reader.num_chunks()) {
             return std::unexpected(ExpectedError("Chunk index " + std::to_string(index) + " is out of bounds."));
        }
        auto chunk_result = reader.get_chunk(index);
        if (!chunk_result) return std::unexpected(ExpectedError(chunk_result.error()));
        
        total_decoded_size += chunk_result->expected_size();
        chunks.push_back(std::move(std::make_unique<Chunk>(std::move(*chunk_result))));

        // Validate chunk compatibility for final_shape calculation
        const auto& current_chunk_shape = chunks.back()->get_shape();
        if (!first_dtype) {
            first_dtype = chunks.back()->dtype();
            if (!current_chunk_shape.empty()) {
                sum_first_dim = current_chunk_shape[0];
                first_shape_tail = std::vector<int64_t>(current_chunk_shape.begin() + 1, current_chunk_shape.end());
            } else {
                // Concatenating 0-D arrays results in a 1-D array of scalars
                sum_first_dim = 1; 
                first_shape_tail = std::vector<int64_t>{};
            }
        } else {
            if (chunks.back()->dtype() != *first_dtype) {
                compatible_shapes = false;
            }
            if (!current_chunk_shape.empty()) {
                std::vector<int64_t> current_shape_tail(current_chunk_shape.begin() + 1, current_chunk_shape.end());
                if (current_shape_tail != *first_shape_tail) {
                    compatible_shapes = false;
                }
                sum_first_dim += current_chunk_shape[0];
            } else if (!first_shape_tail->empty()) { // current is 0-D, but first was not
                compatible_shapes = false;
            } else { // Both are 0-D
                sum_first_dim += 1;
            }
        }
    }

    if (total_decoded_size > output_data.size()) {
        return std::unexpected(ExpectedError("Output buffer is too small. Required: " + std::to_string(total_decoded_size) + ", Provided: " + std::to_string(output_data.size())));
    }

    size_t current_offset = 0;
    for (auto& chunk : chunks) {
        auto buffer_result = extractor.read_chunk(*chunk);
        if (!buffer_result) return std::unexpected(ExpectedError(buffer_result.error().to_string()));
        
        auto decoded_span = (*buffer_result)->as_bytes();
        if (request.check_checksums)
        {
            const auto hash = calculate_blake3_hash128(decoded_span);
            if (hash != chunk->hash())
            {
                return std::unexpected(ExpectedError("Checksum mismatch for chunk at offset " + std::to_string(current_offset) + "."));
            }
        }
        std::memcpy(output_data.data() + current_offset, decoded_span.data(), decoded_span.size());
        current_offset += decoded_span.size();
    }
    
    response.client_key = request.client_key;
    response.bytes_written_to_output = current_offset;

    if (compatible_shapes && first_dtype.has_value()) {
        std::vector<int64_t> final_shape;
        if (first_shape_tail->empty()) { // Concatenating 0-D arrays
            final_shape.push_back(sum_first_dim);
        } else {
            final_shape.push_back(sum_first_dim); // First dimension is sum of first dimensions
            final_shape.insert(final_shape.end(), first_shape_tail->begin(), first_shape_tail->end());
        }
        response.final_shape = final_shape;
    } else {
        // If shapes are incompatible, final_shape is omitted (nullopt)
        // As per Option 2.A (Strict), we should fail if incompatible.
        // However, the plan states "final_shape field in the response is null (or omitted)".
        // Let's stick to the plan's recommendation for now, which is lenient.
        // If Option 2.A (Strict) is chosen, this block would be an error return.
    }
    
    return response;
}

} // namespace cryptodd::ffi
