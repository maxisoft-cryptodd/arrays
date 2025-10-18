#pragma once
#include "operation_handler.h"

namespace cryptodd::ffi
{

    class GetUserMetadataHandler final : public IOperationHandler
    {
    public:
        std::expected<nlohmann::json, ExpectedError> execute(CddContext& context, const nlohmann::json& op_request,
                                                             std::span<const std::byte> input_data,
                                                             std::span<std::byte> output_data) override;
    };

    class SetUserMetadataHandler final : public IOperationHandler
    {
    public:
        std::expected<nlohmann::json, ExpectedError> execute(CddContext& context, const nlohmann::json& op_request,
                                                             std::span<const std::byte> input_data,
                                                             std::span<std::byte> output_data) override;
    };

} // namespace cryptodd::ffi
