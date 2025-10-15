#pragma once

#include <vector>

// Conditionally include mimalloc headers if USE_MIMALLOC is defined by CMake.
#ifdef USE_MIMALLOC
#include <mimalloc.h>
#endif

namespace cryptodd::memory
{

#ifdef USE_MIMALLOC
    // When mimalloc is enabled, use mi_stl_allocator for this vector alias.
    // This provides mimalloc's performance benefits to this specific vector type.
    template<typename T>
    using vector = std::vector<T, ::mi_stl_allocator<T>>;
#else
    // When mimalloc is disabled, fall back to the standard std::vector.
    template<typename T>
    using vector = std::vector<T>;
#endif

template<typename T>
vector<T> create_aligned_vector(const size_t size, const size_t alignment)
{
    static_assert(std::is_arithmetic_v<T>);
    uintptr_t uintptr_t_size = size * sizeof(T) / sizeof(uintptr_t);
    if (uintptr_t_size % alignment != 0)
    {
        uintptr_t_size += alignment - uintptr_t_size % alignment;
    }
    auto res = vector<T>(uintptr_t_size * sizeof(uintptr_t) / sizeof(T));
    res.resize(size);
    return res;
}

}
