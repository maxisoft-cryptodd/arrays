#include "inspect_handler.h"
#include <magic_enum/magic_enum.hpp>
#include "../../file_format/cdd_file_format.h"

namespace cryptodd::ffi {

std::expected<nlohmann::json, ExpectedError> InspectHandler::execute(CddContext& context,
    const nlohmann::json&, std::span<const std::byte>, std::span<std::byte>)
{
    auto reader_opt = context.get_reader();
    if (!reader_opt) return std::unexpected(ExpectedError("Context is not in a readable mode."));
    DataReader& reader = reader_opt.value().get();

    nlohmann::json result;
    result["total_chunks"] = reader.num_chunks();

    nlohmann::json chunk_summaries = nlohmann::json::array();
    for (size_t i = 0; i < reader.num_chunks(); ++i) {
        auto chunk_result = reader.get_chunk(i);
        if (!chunk_result) return std::unexpected(ExpectedError(chunk_result.error()));
        
        nlohmann::json summary;
        summary["index"] = i;
        summary["shape"] = chunk_result->get_shape();
        summary["dtype"] = magic_enum::enum_name(chunk_result->dtype());
        summary["codec"] = magic_enum::enum_name(chunk_result->type());
        summary["encoded_size_bytes"] = chunk_result->data().size();
        summary["decoded_size_bytes"] = chunk_result->expected_size();
        chunk_summaries.push_back(summary);
    }
    result["chunk_summaries"] = chunk_summaries;
    return result;
}
}
