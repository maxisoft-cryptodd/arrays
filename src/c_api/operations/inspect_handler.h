#pragma once
#include "../operations/operation_handler.h"
#include "../operations/operation_types.h"
#include <nlohmann/json_fwd.hpp>
#include <span>

namespace cryptodd::ffi {
class InspectHandler final : public IOperationHandler {
public:
    std::expected<nlohmann::json, ExpectedError> execute(
        CddContext& context, const nlohmann::json& op_request,
        std::span<const std::byte> input_data, std::span<std::byte> output_data) override;
private:
    std::expected<InspectResponse, ExpectedError> execute_typed(
        CddContext& context, const InspectRequest& request);
};
} // namespace cryptodd::ffi
