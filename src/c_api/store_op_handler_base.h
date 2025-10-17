#pragma once

#include "../c_api/operation_handler.h"
#include "../c_api/c_api_utils.h"

namespace cryptodd::ffi {

class StoreOperationHandlerBase : public IOperationHandler {
protected:
    // This function contains the shared logic for processing and writing one chunk.
    // It's called once by StoreChunkHandler and in a loop by StoreArrayHandler.
    std::expected<nlohmann::json, ExpectedError> compress_and_write_chunk(
        CddContext& context,
        DataWriter& writer,
        const utils::DataSpec& data_spec,
        const nlohmann::json& encoding_json,
        std::span<const std::byte> chunk_input_data
    );
};

} // namespace cryptodd::ffi
