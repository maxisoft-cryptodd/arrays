#pragma once

#include "../c_api/cdd_context.h"
#include <nlohmann/json.hpp>
#include <expected>
#include <span>

namespace cryptodd::ffi {

class IOperationHandler {
public:
    virtual ~IOperationHandler() = default;

    virtual std::expected<nlohmann::json, ExpectedError> execute(
        CddContext& context,
        const nlohmann::json& op_request,
        std::span<const std::byte> input_data,
        std::span<const std::byte> output_data) = 0;
};

} // namespace cryptodd::ffi
