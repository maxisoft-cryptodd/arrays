#pragma once

#include <string>
#include <vector>
#include <cstddef>

#include "../memory/allocator.h"

namespace cryptodd::ffi::base64 {

std::string encode(const memory::vector<std::byte>& data);
memory::vector<std::byte> decode(const std::string_view& input);

} // namespace cryptodd::ffi::base64
