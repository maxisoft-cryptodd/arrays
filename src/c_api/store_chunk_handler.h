#pragma once

#include "../c_api/operation_handler.h"

namespace cryptodd::ffi {

class StoreChunkHandler final : public IOperationHandler {
public:
    std::expected<nlohmann::json, ExpectedError> execute(
        CddContext& context,
        const nlohmann::json& op_request,
        std::span<const std::byte> input_data,
        std::span<const std::byte> output_data) override;
};

} // namespace cryptodd::ffi
