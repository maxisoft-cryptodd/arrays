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

}
