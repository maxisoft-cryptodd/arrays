#pragma once
#include <cstddef>      // std::size_t, std::ptrdiff_t
#include <limits>       // std::numeric_limits
#include <new>          // ::operator new, ::operator delete
#include <utility>      // std::forward
#include <type_traits>  // std::true_type, std::false_type

// Use [[nodiscard]] if available (C++17+)
#if (__cplusplus >= 201703L)
#define CUSTOM_ALLOC_NODISCARD [[nodiscard]]
#else
#define CUSTOM_ALLOC_NODISCARD
#endif
#include <vector>
#include <cstddef>
// ReSharper disable once CppUnusedIncludeDirective
#include <cstdint>
#include <type_traits>


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

    /**
 * @brief Common base for the CustomAllocator.
 *
 * This struct provides all the common typedefs and member functions required by the
 * C++ Allocator concept. By placing them in a base class, we keep the main
 * allocator class clean and focused on allocation/deallocation logic.
 * This pattern is inspired by the mimalloc STL allocator.
 */
template<class T>
struct _custom_allocator_common {
  // --- Standard Allocator Typedefs ---
  // These are required by the Allocator concept. std::allocator_traits
  // can provide defaults for some of them, but defining them explicitly is good practice.
  using value_type      = T;
  using size_type       = std::size_t;
  using difference_type = std::ptrdiff_t;
  using reference       = value_type&;
  using const_reference = const value_type&;
  using pointer         = value_type*;
  using const_pointer   = const value_type*;

  // --- C++11 Allocator Traits ---
  // These traits inform STL containers how to handle this allocator during
  // container copy, move, and swap operations. For a stateless allocator,
  // they should all be true_type.
#if ((__cplusplus >= 201103L) || (_MSC_VER > 1900))
  using propagate_on_container_copy_assignment = std::true_type;
  using propagate_on_container_move_assignment = std::true_type;
  using propagate_on_container_swap            = std::true_type;

  // C++11 construct uses variadic templates and perfect forwarding.
  template <class U, class ...Args>
  void construct(U* p, Args&& ...args) {
    // Use placement new to construct the object in-place.
    ::new(static_cast<void*>(p)) U(std::forward<Args>(args)...);
  }

  // C++11 destroy is a noexcept function.
  template <class U>
  void destroy(U* p) noexcept {
    p->~U();
  }
#else
  // Pre-C++11 versions of construct and destroy.
  void construct(pointer p, const_reference val) {
    ::new(static_cast<void*>(p)) value_type(val);
  }

  void destroy(pointer p) {
    p->~value_type();
  }
#endif

  // --- Utility Functions ---
  [[nodiscard]] size_type max_size() const noexcept {
    // Return the maximum number of elements that can be allocated.
    return std::numeric_limits<size_type>::max() / sizeof(value_type);
  }

  // ReSharper disable once CppMemberFunctionMayBeStatic
  pointer address(reference x) const noexcept {
    return &x;
  }

  // ReSharper disable once CppMemberFunctionMayBeStatic
  const_pointer address(const_reference x) const noexcept {
    return &x;
  }
};


/**
 * @brief A general-purpose custom allocator.
 *
 * It is designed to be a feature complete drop-in replacement for std::allocator.
 */
template<class T>
struct DefaultAllocator : public _custom_allocator_common<T> {
#ifdef USE_MIMALLOC
    using DelegateAllocator = ::mi_stl_allocator<T>;
#else
    using DelegateAllocator = std::allocator<T>;
#endif

private:

    DelegateAllocator allocator_;
public:
  // --- Bring typedefs from base into scope ---
  using typename _custom_allocator_common<T>::size_type;
  using typename _custom_allocator_common<T>::value_type;
  using typename _custom_allocator_common<T>::pointer;

  // --- Rebind Mechanism ---
  // Allows a container to obtain an allocator for a different type.
  // For example, std::list<T, Alloc<T>> needs to allocate internal nodes of type
  // Node<T>, so it uses rebind to get an Alloc<Node<T>>.
  template <class U>
  struct rebind {
    using other = DefaultAllocator<U>;
  };

  // --- Constructors ---
  // A stateless allocator's constructors do nothing.
  DefaultAllocator() noexcept = default;
  DefaultAllocator(const DefaultAllocator&) noexcept = default;
  template<class U>
  explicit DefaultAllocator(const DefaultAllocator<U>&) noexcept { }

  // --- Member Functions ---
  // This function is called by containers during copy construction. For a stateless
  // allocator, we can just return a default-constructed instance.
  DefaultAllocator select_on_container_copy_construction() const {
    return DefaultAllocator();
  }

  // The core allocation function.
  CUSTOM_ALLOC_NODISCARD
  pointer allocate(size_type count) {
    if (count > this->max_size()) {
      throw std::bad_alloc();
    }
    // For this example, we simply wrap the global operator new.
    // This is where you would plug in a custom memory pool or another backend.
    return static_cast<pointer>(allocator_.allocate(count));
  }

  // The core deallocation function.
  void deallocate(T* p, size_type count) noexcept {
    // The `count` parameter is unused here but must be present in the signature.
    // It can be used as a hint for sized deallocation in C++14+.
    allocator_.deallocate(p, count);
  }

  // --- C++11 is_always_equal Trait ---
  // This is a key optimization. It tells the STL that any two instances of
  // DefaultAllocator<T> are interchangeable, avoiding unnecessary allocator
  // copies or moves during container operations.
#if ((__cplusplus >= 201103L) || (_MSC_VER > 1900))
  using is_always_equal = std::true_type;
#endif
};


// --- Global Equality Operators ---
// For a stateless allocator, all instances are equal.
// These operators are required by the Allocator concept.
template<class T1, class T2>
bool operator==(const DefaultAllocator<T1>&, const DefaultAllocator<T2>&) noexcept {
  return true;
}

template<class T1, class T2>
bool operator!=(const DefaultAllocator<T1>&, const DefaultAllocator<T2>&) noexcept {
  return false;
}

}
