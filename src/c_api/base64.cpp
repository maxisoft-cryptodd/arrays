#include "base64.h"

#include <cassert>
#include <expected>
#include <stdexcept>

#include <turbobase64/turbob64.h>

namespace cryptodd::ffi::base64
{
    namespace
    {
        inline std::expected<std::span<char>, Error> encode_impl(std::span<const std::byte> data, std::span<char> output)
        {
            if (data.empty())
            {
                return std::span<char>{};
            }

            const auto required_size = tb64enclen(data.size());
            if (required_size > output.size())
            {
                return std::unexpected(Error{ErrorCode::OutputBufferTooSmall, required_size});
            }

            static_assert(sizeof(std::byte) == sizeof(unsigned char), "std::byte must be compatible with unsigned char");
            static_assert(sizeof(char) == sizeof(unsigned char), "char must be compatible with unsigned char");

            const auto encoded_size = tb64enc(reinterpret_cast<const unsigned char*>(data.data()), data.size(),
                                              reinterpret_cast<unsigned char*>(output.data()));

            if (encoded_size == 0 && !data.empty())
            {
                // This indicates an internal library failure, not just invalid input.
                return std::unexpected(Error{ErrorCode::InvalidInput, 0});
            }

            return output.subspan(0, encoded_size);
        }

        // Core decoding logic, assumes output buffer is sufficiently large.
        inline std::expected<std::span<std::byte>, Error> decode_into(const std::string_view& input,
                                                                      std::span<std::byte> output)
        {
            static_assert(sizeof(char) == sizeof(unsigned char), "char must be compatible with unsigned char");
            static_assert(sizeof(std::byte) == sizeof(unsigned char), "std::byte must be compatible with unsigned char");

            const auto decoded_size = tb64dec(reinterpret_cast<const unsigned char*>(input.data()), input.size(),
                                              reinterpret_cast<unsigned char*>(output.data()));

            if (decoded_size == 0 && !input.empty())
            {
                // This can happen if tb64declen passes but tb64dec finds an issue (e.g., bad padding).
                return std::unexpected(Error{ErrorCode::InvalidInput, 0});
            }

            return output.subspan(0, decoded_size);
        }

        inline std::expected<std::span<std::byte>, Error> decode_impl(const std::string_view& input,
                                                                      std::span<std::byte> output)
        {
            if (input.empty())
            {
                return std::span<std::byte>{};
            }

            static_assert(sizeof(char) == sizeof(unsigned char), "char must be compatible with unsigned char");

            const auto required_size = tb64declen(reinterpret_cast<const unsigned char*>(input.data()), input.size());
            if (required_size == 0 && !input.empty())
            {
                // Indicates decode failure (e.g., invalid characters).
                return std::unexpected(Error{ErrorCode::InvalidInput, 0});
            }

            if (required_size > output.size_bytes())
            {
                return std::unexpected(Error{ErrorCode::OutputBufferTooSmall, required_size});
            }

            return decode_into(input, output);
        }

    } // namespace

    std::string encode(const std::span<const std::byte> data)
    {
        if (data.empty())
        {
            return {};
        }

        std::string ret;
        const auto required_size = tb64enclen(data.size());
        ret.resize_and_overwrite(required_size, [&](char* out_ptr, size_t out_size) {
            auto result = encode_impl(data, {out_ptr, out_size});
            if (!result)
            {
                // A return of 0 from tb64enc with non-empty input is an exceptional failure.
                throw std::runtime_error("Base64 encode failed (returned 0).");
            }
            return result->size();
        });

        return ret;
    }

    std::expected<std::span<char>, Error> encode(std::span<const std::byte> data, std::span<char> output) { return encode_impl(data, output); }

    memory::vector<std::byte> decode(const std::string_view& input)
    {
        if (input.empty())
        {
            return {};
        }

        static_assert(sizeof(char) == sizeof(unsigned char), "char must be compatible with unsigned char");

        const auto required_size = tb64declen(reinterpret_cast<const unsigned char*>(input.data()), input.size());
        if (required_size == 0 && !input.empty())
        {
            // Invalid base64 input that decodes to zero bytes.
            throw std::runtime_error("Base64 decode failed: invalid input.");
        }

        memory::vector<std::byte> ret(required_size);
        auto result = decode_into(input, ret);

        if (!result)
        {
            // This path is highly unlikely if tb64declen succeeded and we allocated enough space.
            // It implies a logic error or that tb64dec failed for other reasons (e.g. invalid chars).
            throw std::runtime_error("Base64 decode failed during final decoding step.");
        }

        ret.resize(result->size());
        return ret;
    }

    std::expected<std::span<std::byte>, Error> decode(const std::string_view& input, std::span<std::byte> output)
    {
        return decode_impl(input, output);
    }

} // namespace cryptodd::ffi::base64
