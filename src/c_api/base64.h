#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "../memory/allocator.h"

namespace cryptodd::ffi::base64
{
    enum class ErrorCode
    {
        InvalidInput,
        OutputBufferTooSmall
    };

    struct Error
    {
        ErrorCode code;
        size_t required_size = 0;

        bool operator==(const Error&) const = default;
    };

    std::string encode(std::span<const std::byte> data);
    std::expected<std::span<char>, Error> encode(std::span<const std::byte> data, std::span<char> output);

    memory::vector<std::byte> decode(const std::string_view& input);
    std::expected<std::span<std::byte>, Error> decode(const std::string_view& input, std::span<std::byte> output);

} // namespace cryptodd::ffi::base64
