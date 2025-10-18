#include "store_chunk_handler.h"
#include <numeric>
#include "../c_api_utils.h"

namespace cryptodd::ffi {

using namespace utils;

std::expected<nlohmann::json, ExpectedError> StoreChunkHandler::execute(
    CddContext& context,
    const nlohmann::json& op_request,
    std::span<const std::byte> input_data,
    std::span<std::byte> /*output_data*/)
{
    auto writer_opt = context.get_writer();
    if (!writer_opt) {
        return std::unexpected(ExpectedError("Context is not in a writable mode."));
    }
    DataWriter& writer = writer_opt.value().get();

    auto data_spec_result = get_required<nlohmann::json>(op_request, "data_spec").and_then(parse_data_spec);
    if (!data_spec_result) return std::unexpected(data_spec_result.error());
    
    const size_t expected_bytes =
        std::accumulate(data_spec_result->shape.begin(), data_spec_result->shape.end(), size_t{1}, std::multiplies<>()) *
        get_dtype_size(data_spec_result->dtype);
    if (input_data.size() != expected_bytes) {
        return std::unexpected(ExpectedError("Input data size does not match shape and dtype specification."));
    }

    const auto& encoding_json = op_request.at("encoding");

    auto result = compress_and_write_chunk(context, writer, *data_spec_result, encoding_json, input_data);
    if (!result) return result;

    // Add top-level summary info
    (*result)["chunks_written"] = 1;
    (*result)["shape"] = data_spec_result->shape;
    (*result)["zstd_level"] = encoding_json.value("zstd_level", ZstdCompressor::DEFAULT_COMPRESSION_LEVEL);

    return result;
}

} // namespace cryptodd::ffi
