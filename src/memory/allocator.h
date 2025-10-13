#pragma once

#include <mimalloc.h>

#include <vector>

namespace cryptodd::memory
{
    template<typename T>
    using vector = std::vector<T, ::mi_stl_allocator<T>>;
}

