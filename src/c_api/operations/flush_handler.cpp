#include "flush_handler.h"

namespace cryptodd::ffi {

std::expected<nlohmann::json, ExpectedError> FlushHandler::execute(CddContext& context,
    const nlohmann::json&, std::span<const std::byte>, std::span<std::byte>)
{
    auto writer_opt = context.get_writer();
    if (!writer_opt) return std::unexpected(ExpectedError("Context is not in a writable mode."));
    
    auto result = writer_opt.value().get().flush();
    if (!result) return std::unexpected(ExpectedError(result.error()));

    return nlohmann::json{{"status", "Flush completed."}};
}

}
