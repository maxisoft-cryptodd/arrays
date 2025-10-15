#pragma once

#include "../memory/allocator.h" // Includes vector alias and mimalloc headers if needed

#include <cassert>
#include <cstddef>     // For std::size_t
#include <new>         // For std::bad_alloc, std::align_val_t
#include <type_traits> // For std::is_arithmetic_v

#ifndef USE_MIMALLOC
// Only include the Highway header when it's the chosen fallback.
#include <hwy/aligned_allocator.h>
#endif

namespace cryptodd::memory
{

#ifdef USE_MIMALLOC
    // --- IMPLEMENTATION 1: Use mimalloc for aligned allocation ---

    // A specialized C++ allocator that uses mimalloc's C++-style aligned API.
template <typename T, std::size_t Alignment>
struct MiAlignedAllocator
{
    using value_type = T;
    static constexpr std::size_t alignment = Alignment;

    static_assert((Alignment > 0) && ((Alignment & (Alignment - 1)) == 0), "Alignment must be a power of two.");

    template <class U>
    struct rebind
    {
        using other = MiAlignedAllocator<U, Alignment>;
    };

    MiAlignedAllocator() noexcept = default;

    template <typename U>
    MiAlignedAllocator(const MiAlignedAllocator<U, Alignment> &) noexcept
    {
    }

    [[nodiscard]] T *allocate(std::size_t n)
    {
        if (n > std::size_t(-1) / sizeof(T))
        {
            throw std::bad_alloc();
        }
        const std::size_t bytes = n * sizeof(T);

        void *p = mi_new_aligned(bytes, Alignment);
        assert(reinterpret_cast<std::uintptr_t>(p) % Alignment == 0);
        if (!p)
        {
            throw std::bad_alloc();
        }
        return static_cast<T *>(p);
    }

    void deallocate(T *p, std::size_t n) noexcept
    {
        (void)n;
        mi_free(p);
    }
};

template <typename T, std::size_t TA, typename U, std::size_t UA>
bool operator==(const MiAlignedAllocator<T, TA> &, const MiAlignedAllocator<U, UA> &) noexcept
{
    return TA == UA;
}

template <typename T, std::size_t TA, typename U, std::size_t UA>
bool operator!=(const MiAlignedAllocator<T, TA> &, const MiAlignedAllocator<U, UA> &) noexcept
{
    return TA != UA;
}

template <typename T, std::size_t Alignment>
using AlignedAllocator = MiAlignedAllocator<T, Alignment>;

#else
    // --- IMPLEMENTATION 2: Use Google Highway's allocator as a fallback ---

    // hwy::AlignedAllocator<T> is already a complete C++ allocator.
    // However, it doesn't take an alignment parameter. It uses a fixed HWY_ALIGNMENT.
    // We create a wrapper that inherits from it to provide a consistent interface.
template <typename T, std::size_t Alignment>
struct HwyAlignedAllocator : public hwy::AlignedAllocator<T> {

    static_assert(Alignment <= HWY_ALIGNMENT,
                  "When using the Highway backend, memory will be aligned to HWY_ALIGNMENT "
                  "(typically 128 bytes). Requesting a larger alignment is not supported.");

    template<class U>
    struct rebind {
        using other = HwyAlignedAllocator<U, Alignment>;
    };

    HwyAlignedAllocator() noexcept = default;
    template<class U>
    HwyAlignedAllocator(const HwyAlignedAllocator<U, Alignment>&) noexcept {}
};

template <typename T, std::size_t TA, typename U, std::size_t UA>
bool operator==(const HwyAlignedAllocator<T, TA>&, const HwyAlignedAllocator<U, UA>&) noexcept {
    return true; // hwy::AlignedAllocator is stateless
}

template <typename T, std::size_t TA, typename U, std::size_t UA>
bool operator!=(const HwyAlignedAllocator<T, TA>&, const HwyAlignedAllocator<U, UA>&) noexcept {
    return false;
}

template <typename T, std::size_t Alignment>
using AlignedAllocator = HwyAlignedAllocator<T, Alignment>;

#endif

    // Define the AlignedVector type alias. This works for BOTH implementations above.
    template <typename T, std::size_t Alignment>
    using AlignedVector = std::vector<T, AlignedAllocator<T, Alignment>>;


    // Define the factory function. This also works for BOTH implementations.
    template<typename T, std::size_t Alignment>
    AlignedVector<T, Alignment> create_aligned_vector(const std::size_t size)
    {
        static_assert(std::is_arithmetic_v<T>);
        return AlignedVector<T, Alignment>(size);
    }

} // namespace cryptodd::memory