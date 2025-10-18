#pragma once

#include <expected>
#include <nlohmann/json.hpp>
#include <span>
#include "../cdd_context.h"

namespace cryptodd::ffi
{

    class IOperationHandler
    {
    public:
        virtual ~IOperationHandler() = default;

        virtual std::expected<nlohmann::json, ExpectedError> execute(CddContext& context,
                                                                     const nlohmann::json& op_request,
                                                                     std::span<const std::byte> input_data,
                                                                     std::span<std::byte> output_data) = 0;
    };

} // namespace cryptodd::ffi
