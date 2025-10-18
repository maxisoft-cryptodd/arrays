#include "base64.h"

#include <cassert>
#include <stdexcept>

#include <turbobase64/turbob64.h>

namespace cryptodd::ffi::base64
{

    std::string encode(const memory::vector<std::byte>& data)
    {
        if (data.empty())
        {
            return {};
        }
        std::string ret;
        ret.resize(tb64enclen(data.size()));
        // Ensure that the value_type of the vector is compatible with what turbob64 expects.
        static_assert(sizeof(unsigned char) == sizeof(std::remove_reference_t<decltype(data)>::value_type));
        const auto size = tb64enc(reinterpret_cast<const unsigned char*>(data.data()), data.size(),
                                  reinterpret_cast<unsigned char*>(ret.data()));
        // The library returns 0 on error for encoding, though it's unlikely with valid inputs.
        if (size == 0)
        {
            throw std::runtime_error("Base64 encode failed (returned 0).");
        }
        assert(size <= ret.size());
        ret.resize(size);
        return ret;
    }

    // NOTE: A full decode implementation is omitted for brevity but would be included in a production system.
    // For now, we only need encode for GetUserMetadata.
    memory::vector<std::byte> decode(const std::string_view& input)
    {
        if (input.empty())
        {
            return {};
        }
        // Ensure that the value_type of the vector is compatible with what turbob64 expects.
        static_assert(sizeof(unsigned char) == sizeof(std::remove_reference_t<decltype(input)>::value_type));
        auto size = tb64declen(reinterpret_cast<const unsigned char*>(input.data()), input.size());
        // tb64declen returns the decoded size. It doesn't have a documented error return,
        // but we can proceed and let tb64dec handle the validation.

        if (size == 0)
        {
            return {};
        }

        memory::vector<std::byte> ret{size, std::byte()};
        const auto dec_size = tb64dec(reinterpret_cast<const unsigned char*>(input.data()), input.size(),
                                      reinterpret_cast<unsigned char*>(ret.data()));
        // Per documentation, tb64dec returns 0 on error.
        if (dec_size == 0 && !input.empty())
        {
            // Throw a specific, descriptive exception.
            throw std::runtime_error("Base64 decode failed: invalid input data or length.");
        }

        assert(dec_size <= ret.size());
        ret.resize(dec_size);

        return ret;
    }


} // namespace cryptodd::ffi::base64
