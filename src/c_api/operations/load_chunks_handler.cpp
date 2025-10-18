#include "load_chunks_handler.h"
#include <numeric>
#include "../c_api_utils.h"

namespace cryptodd::ffi {

using namespace utils;

std::expected<nlohmann::json, ExpectedError> LoadChunksHandler::execute(CddContext& context,
    const nlohmann::json& op_request, std::span<const std::byte>, std::span<std::byte> output_data)
{
    auto reader_opt = context.get_reader();
    if (!reader_opt) return std::unexpected(ExpectedError("Context is not in a readable mode."));
    DataReader& reader = reader_opt.value().get();
    DataExtractor& extractor = context.get_extractor();

    // 1. Parse selection to get list of indices
    const auto& selection = op_request.at("selection");
    const auto type = selection.at("type").get<std::string>();
    std::vector<size_t> indices_to_load;

    if (type == "All") {
        indices_to_load.resize(reader.num_chunks());
        std::iota(indices_to_load.begin(), indices_to_load.end(), 0);
    } else if (type == "Indices") {
        indices_to_load = selection.at("indices").get<std::vector<size_t>>();
    } else if (type == "Range") {
        size_t start = selection.at("start_index").get<size_t>();
        size_t count = selection.at("count").get<size_t>();
        for (size_t i = 0; i < count && start + i < reader.num_chunks(); ++i) {
            indices_to_load.push_back(start + i);
        }
    } else {
        return std::unexpected(ExpectedError("Unknown selection type: " + type));
    }

    if (indices_to_load.empty()) return nlohmann::json{{"bytes_written_to_output", 0}};

    // 2. Pre-flight check: Calculate total size and validate
    size_t total_decoded_size = 0;
    std::vector<Chunk> chunks;
    chunks.reserve(indices_to_load.size());
    for (const auto index : indices_to_load) {
        auto chunk_result = reader.get_chunk(index);
        if (!chunk_result) return std::unexpected(ExpectedError(chunk_result.error()));
        total_decoded_size += chunk_result->expected_size();
        chunks.push_back(std::move(*chunk_result));
    }

    if (total_decoded_size > output_data.size()) {
        return std::unexpected(ExpectedError("Output buffer is too small. Required: " + std::to_string(total_decoded_size) + ", Provided: " + std::to_string(output_data.size())));
    }

    // 3. Decode and copy data
    size_t current_offset = 0;
    for (auto& chunk : chunks) {
        auto buffer_result = extractor.read_chunk(chunk);
        if (!buffer_result) return std::unexpected(ExpectedError(buffer_result.error().to_string()));
        
        auto decoded_span = (*buffer_result)->as_bytes();
        std::memcpy(output_data.data() + current_offset, decoded_span.data(), decoded_span.size());
        current_offset += decoded_span.size();
    }
    
    nlohmann::json result;
    result["bytes_written_to_output"] = current_offset;
    // TODO: Calculate final shape based on concatenated chunks
    return result;
}
}
