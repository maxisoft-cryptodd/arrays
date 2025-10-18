#pragma once

#include "store_op_handler_base.h"

namespace cryptodd::ffi
{

    class StoreArrayHandler final : public StoreOperationHandlerBase
    {
    public:
        std::expected<nlohmann::json, ExpectedError> execute(CddContext& context, const nlohmann::json& op_request,
                                                             std::span<const std::byte> input_data,
                                                             std::span<std::byte> output_data) override;
    };

} // namespace cryptodd::ffi
