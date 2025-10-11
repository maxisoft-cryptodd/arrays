Of course. Here is an exhaustive, condensed, and deeply detailed documentation for the Google Highway library, designed to serve as an ultimate reference for experienced C++ developers. This guide synthesizes all provided materials, including the API references, design rationale, code examples like the `orderbook_simd_codec`, and the instruction support matrix.

***

# The Ultimate Highway Documentation for C++ Developers

## Table of Contents

1.  **Core Philosophy and Design Rationale**
    *   1.1. Guiding Principles
    *   1.2. Why Not Auto-Vectorization, Assembly, or Raw Intrinsics?
    *   1.3. Key Design Decisions: Sizeless Vectors, Safe Dispatch, API Compactness
2.  **Project Setup and Dispatching**
    *   2.1. Integration (CMake, Bazel, Manual)
    *   2.2. The Multi-Target Boilerplate: `foreach_target.h` and `HWY_NAMESPACE`
    *   2.3. Static vs. Dynamic Dispatch: Choosing the Right Strategy
3.  **Fundamental Concepts**
    *   3.1. The Tag-Dispatch System: `D` vs. `V`
    *   3.2. Vector and Lane Management
    *   3.3. Memory Model: Alignment, Allocation, and Data Layout (AoS vs. SoA)
4.  **Exhaustive Core API Reference**
    *   4.1. Notation
    *   4.2. Initialization and Constants
    *   4.3. Memory Operations
    *   4.4. Arithmetic Operations
    *   4.5. Logical and Bitwise Operations
    *   4.6. Comparisons and Masks
    *   4.7. Control Flow and Predication
    *   4.8. Type Conversions and Promotions
    *   4.9. Swizzle, Permute, and Shuffle Operations
    *   4.10. Reductions (Horizontal Operations)
    *   4.11. Specialized and Cryptographic Operations
5.  **The `contrib` Ecosystem: High-Level Modules**
    *   5.1. `sort`: High-Performance Vectorized Sorting (`VQSort`)
    *   5.2. `matvec`: Matrix-Vector Multiplication
    *   5.3. `bit_pack`: Efficient Bit-Packing and Unpacking
    *   5.4. `random`: SIMD-Accelerated Random Number Generation
    *   5.5. `image`: Aligned Image Plane Management
    *   5.6. `unroller`: Automated Loop Unrolling and Optimization
    *   5.7. `thread_pool`: A High-Performance, NUMA-Aware Thread Pool
6.  **Performance, Pitfalls, and Best Practices**
    *   6.1. Do's and Don'ts: A Quick Reference
    *   6.2. Understanding Floating-Point Behavior
    *   6.3. AVX Throttling and Startup Costs: A Deep Dive
    *   6.4. Debugging and Verification
7.  **Case Study: `orderbook_simd_codec`**
    *   7.1. Algorithm Overview: Delta Encoding and Data Shuffling
    *   7.2. `encode16`: Demotion, XOR, and Shuffle for Compressibility
    *   7.3. `decode16`: Unshuffling, Reconstruction, and Promotion
    *   7.4. The `float32` Path: When to Avoid Demotion
8.  **Instruction Portability Matrix**
    *   8.1. Interpreting the Matrix
    *   8.2. Table of Instruction Counts per Platform
    *   8.3. Analysis and Portability Implications
9.  **Advanced Topics and Internals**
    *   9.1. Preprocessor Macros and Configuration
    *   9.2. Extending Highway: Adding New Operations
    *   9.3. Type Traits and Metaprogramming Helpers

---

## 1. Core Philosophy and Design Rationale

Highway is engineered to solve the practical challenges of writing high-performance, portable SIMD code in C++. It abstracts away architectural differences without sacrificing performance, providing a stable and predictable foundation.

### 1.1. Guiding Principles

*   **Performance is Paramount, but Not Solitary**: The primary motivation for SIMD is speed. Highway aims for performance within 10-20% of hand-tuned assembly, offering a direct mapping to intrinsics with zero overhead. However, it balances this with portability, maintainability, and readability.
*   **Performance Portability**: The API is a carefully curated *intersection* of operations that are efficient on all major targets (x86 SSE4/AVX2/AVX-512, Arm NEON/SVE, POWER VSX, RISC-V V). This prevents developers from unknowingly using an instruction that is fast on their machine but a performance cliff on another.
*   **"Pay Only for What You Use"**: Operation costs are transparent and predictable. There are no hidden conversions or expensive emulations for core operations. Implicit conversions (e.g., integer to float) are disallowed to make their cost visible.
*   **Future-Proof through Width-Agnosticism**: By supporting sizeless vector types (from Arm SVE, RISC-V V), Highway encourages writing code that automatically adapts to future, wider vector registers without modification.
*   **Robustness Above All**: Highway is designed to work around a long history of compiler bugs, poor codegen from auto-vectorizers, and subtle ABI differences that plague raw intrinsic programming.

### 1.2. Why Not Auto-Vectorization, Assembly, or Raw Intrinsics?

| Approach | Drawbacks Addressed by Highway |
| :--- | :--- |
| **Auto-Vectorization** | **Brittle and Unpredictable**: Success is highly dependent on compiler heuristics, which change between versions. It often fails on complex loops and can produce suboptimal code (e.g., "SIMD memcpy" instead of efficient shuffles). |
| **Assembly** | **Error-Prone and Laborious**: Requires manual register allocation, handling of ABI differences, and is extremely difficult to maintain and port (e.g., `FMLA` vs. `vfmadd132ps`). A single mistake, like `MOVAPS` vs. `VMOVAPS`, can incur major performance penalties. |
| **Raw Intrinsics** | **Non-Portable and Verbose**: Code is tied to a specific instruction set (e.g., `_mm256_...`). Porting is a complete rewrite. The syntax is verbose (e.g., `_mm256_load_si256(reinterpret_cast<...>)`) and prone to compiler bugs. |

### 1.3. Key Design Decisions

*   **Tag-Dispatch via Overloaded Functions**: Instead of relying on class templates (which fail for sizeless types like SVE vectors), Highway uses zero-sized "tag" arguments (`Simd<T, N>`) to select the correct overloaded function. This is the cornerstone of its portability.
*   **Safe Runtime Dispatch**: Highway provides a robust mechanism using `foreach_target.h`, target-specific namespaces, and function attributes to compile code for multiple instruction sets within a single binary. This avoids ODR violations and allows safe, efficient runtime selection of the best codepath for the host CPU.
*   **Compact and Curated API**: The API intentionally omits operations that are not performance-portable. It provides a concise set of powerful primitives that map efficiently to all target hardware.

---

## 2. Project Setup and Dispatching

### 2.1. Integration

| Method | Steps |
| :--- | :--- |
| **CMake (Recommended)** | 1. `find_package(HWY 1.3.0)` <br> 2. `target_link_libraries(your_project PRIVATE hwy)` |
| **Bazel** | Add `deps = ["//path/to/highway:hwy"]` to your `cc_library` or `cc_binary`. |
| **Manual / Git Submodule** | 1. Add Highway as a submodule or download the source. <br> 2. Compile `hwy/per_target.cc` and `hwy/targets.cc` with your project. <br> 3. Add the Highway directory to your include paths. |

### 2.2. The Multi-Target Boilerplate

To write portable, dynamically-dispatched SIMD code, your `.cpp` file must follow a specific structure.

**Example `my_simd_module.cpp`:**

```cpp
// 1. Define the file to be re-included for each target. Must be before any other includes.
#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "my_simd_module.cpp"
#include "hwy/foreach_target.h"  // This will re-include the current file.

// 2. Include standard headers and the main highway header.
#include "hwy/highway.h"
#include "my_simd_module.h" // Your header with declarations

// 3. Start the target-specific namespace block.
HWY_BEFORE_NAMESPACE();
namespace my_project {
namespace HWY_NAMESPACE {

// SIMD code goes here. All Highway ops must be prefixed or `using`ed.
namespace hn = hwy::HWY_NAMESPACE;

void MySimdFunctionImpl(const float* HWY_RESTRICT in, float* HWY_RESTRICT out, size_t count) {
  const hn::ScalableTag<float> d;
  for (size_t i = 0; i < count; i += hn::Lanes(d)) {
    const auto v = hn::LoadU(d, in + i);
    hn::StoreU(hn::Mul(v, v), d, out + i);
  }
}

} // namespace HWY_NAMESPACE
} // namespace my_project
HWY_AFTER_NAMESPACE();

// 4. Export the function and define the dispatcher (only compiled once).
#if HWY_ONCE

namespace my_project {

// This defines a function pointer table.
HWY_EXPORT(MySimdFunctionImpl);

// This is the public-facing function that clients will call.
void MySimdFunction(const float* in, float* out, size_t count) {
  // HWY_DYNAMIC_DISPATCH finds the best function pointer for the current CPU and calls it.
  HWY_DYNAMIC_DISPATCH(MySimdFunctionImpl)(in, out, count);
}

} // namespace my_project
#endif // HWY_ONCE
```

### 2.3. Static vs. Dynamic Dispatch

| Dispatch Mode | When to Use | How to Compile | How to Call | Pros / Cons |
| :--- | :--- | :--- | :--- | :--- |
| **Static** | You are targeting a single, known CPU baseline (e.g., all servers have AVX2). | Pass the baseline flag to the compiler (e.g., `-mavx2`). Do **not** use `foreach_target.h`. | Call your `HWY_NAMESPACE` function directly, or via `HWY_STATIC_DISPATCH`. | **+** Smaller binary. <br> **+** No dispatch overhead. <br> **-** Not portable to older CPUs. <br> **-** Misses performance on newer CPUs. |
| **Dynamic** | You need a single binary that runs optimally on a wide range of CPUs (desktops, mobile, diverse cloud servers). | Use the boilerplate from 2.2. Compile without architecture-specific flags (or with flags for the *oldest* supported CPU). | `HWY_DYNAMIC_DISPATCH(FunctionName)`. | **+** Maximum performance portability. <br> **+** Future-proof. <br> **-** Larger binary size. <br> **-** One-time dispatch overhead (negligible). |

---

## 3. Fundamental Concepts

### 3.1. The Tag-Dispatch System: `D` vs. `V`

This is the most critical concept in Highway. You do not operate on vector types directly; you use tags to describe the operation you want.

*   **The Tag (`d`)**: A zero-sized object that describes the desired vector layout. It carries the lane type (`T`) and maximum lane count. It is passed to functions that create or manipulate vectors.
    ```cpp
    const hn::ScalableTag<float> d; // A tag for a scalable vector of floats.
    const hn::FixedTag<uint16_t, 8> d128; // A tag for a 128-bit vector of uint16_t.
    ```

*   **The Vector (`V`)**: The actual data-holding type. Its concrete type is an implementation detail and may be a compiler builtin (`__rvv_float32m1_t`) or a wrapper class (`hwy::Vec128<float>`). **You should almost always use `auto` or `Vec<D>` to declare vector variables.**
    ```cpp
    auto v_zeros = hn::Zero(d); // `v_zeros` is a vector of type V.
    using V = hn::Vec<decltype(d)>; // A portable alias for the vector type.
    ```

**Why this system?** It allows Highway to support sizeless vector types like ARM SVE, which cannot be members of a class and have no compile-time size. The tag provides the necessary type information for overload resolution.

### 3.2. Vector and Lane Management

| Function/Alias | Signature | Description |
| :--- | :--- | :--- |
| `ScalableTag<T>()` | Tag | Creates a tag for the largest vector size supported by the target. **This is the most common tag.** |
| `FixedTag<T, N>()` | Tag | Creates a tag for a vector with exactly `N` lanes. |
| `CappedTag<T, N>()` | Tag | Creates a tag for a vector with at most `N` lanes. |
| `Lanes(d)` | `size_t` | Returns the *actual number of lanes* at runtime for a given tag. **This is NOT a `constexpr` on all targets.** |
| `MaxLanes(d)` | `size_t` | A `constexpr` upper bound on the number of lanes. |
| `TFromD<D>()` | Type | A type trait to get the lane type `T` from a tag type `D`. |

***Do's and Don'ts:***

*   **DO** use `Lanes(d)` to increment loop counters.
*   **DO NOT** use `Lanes(d)` to declare static array sizes. Use `hwy::AllocateAligned`.
*   **DO** use `auto` or `Vec<decltype(d)>` to declare vector variables.
*   **DO NOT** use concrete types like `Vec128<T>` in portable code.
*   **DO NOT** use vectors as class members, function arguments (except by `const&`), or elements of `std::vector` if you need SVE/RVV portability.

### 3.3. Memory Model: Alignment, Allocation, and Data Layout

*   **Alignment**: SIMD loads/stores are fastest when the memory address is aligned to the vector's size in bytes (e.g., 64 bytes for AVX-512). `Load` requires this; `LoadU` (unaligned) does not, but may be slower.
*   **Allocation**: Always use `hwy::AllocateAligned<T>(count)` for heap-allocating arrays that will be accessed by `Load`. It guarantees correct alignment and can help avoid cache conflicts.
*   **Data Layout (AoS vs. SoA)**: For optimal performance, organize data as a **Structure of Arrays (SoA)**, not an Array of Structures (AoS).

**AoS (Bad for SIMD):**
```cpp
struct Point { float x, y, z; };
std::vector<Point> points; // x, y, z are interleaved in memory.
// Loading a vector of just 'x' values requires a slow gather operation.
```

**SoA (Good for SIMD):**
```cpp
struct Points {
  hwy::AlignedFreeUniquePtr<float[]> x, y, z;
  Points(size_t n) : x(hwy::AllocateAligned<float>(n)), ... {}
};
// All 'x' values are contiguous. Loading a vector is a single `Load` instruction.
```

---

## 4. Exhaustive Core API Reference

This section details the primary functions provided by Highway.

### 4.1. Notation

*   `d`: An lvalue of a tag type (e.g., `ScalableTag<float>`).
*   `D`: A tag type (e.g., `decltype(d)`).
*   `v`, `a`, `b`: Variables of a vector type `V`.
*   `m`: A variable of a mask type `M`.
*   `p`, `aligned`: Pointers to memory.
*   `{u,i,f}{8,16,32,64}`: Type constraints. `u`=unsigned, `i`=signed, `f`=float.

### 4.2. Initialization and Constants

| Function | Signature | Description |
| :--- | :--- | :--- |
| `Zero` | `V Zero(D d)` | Returns a vector of all zeros. |
| `Set` | `V Set(D d, T val)` | Broadcasts a scalar `val` to all lanes. |
| `Undefined` | `V Undefined(D d)` | Returns an uninitialized vector. Useful as an out-param. |
| `Iota` | `V Iota(D d, T start)` | `v[i] = start + i`. |
| `SignBit` | `V SignBit(D d)` | Returns a vector where only the sign bit of each lane is set. |
| `Inf` | `V Inf(D d)` | Returns a vector of infinities (float only). |
| `NaN` | `V NaN(D d)` | Returns a vector of quiet NaNs (float only). |
| `GetLane` | `T GetLane(V v)` | Extracts the first lane (lane 0) as a scalar. |
| `ExtractLane` | `T ExtractLane(V v, size_t i)` | Extracts lane `i`. **Slow; avoid in loops.** |
| `InsertLane` | `V InsertLane(V v, size_t i, T val)` | Inserts scalar `val` into lane `i`. **Slow.** |

### 4.3. Memory Operations

| Function | Description | Alignment |
| :--- | :--- | :--- |
| `Load` | Loads a full vector. | Vector-aligned (`hwy::AllocateAligned`). |
| `Store` | Stores a full vector. | Vector-aligned. |
| `LoadU` | Loads a full vector. | Element-aligned (`sizeof(T)`). |
| `StoreU` | Stores a full vector. | Element-aligned. |
| `Stream` | Stores a full vector with a non-temporal hint (bypasses caches). | Vector-aligned. Requires `FlushStream()`. |
| `LoadN` | Loads `count` lanes, zero-padding the rest. Safe for remainders. | Element-aligned. |
| `StoreN` | Stores `count` lanes. Safe for remainders. | Element-aligned. |
| `MaskedLoad` | `MaskedLoad(mask, d, p)`: Loads where `mask` is true, zeros elsewhere. | Element-aligned. |
| `MaskedLoadOr`| `MaskedLoadOr(no, mask, d, p)`: Loads where `mask` is true, uses `no` elsewhere. | Element-aligned. |
| `BlendedStore` | `BlendedStore(v, mask, d, p)`: Reads `*p`, then stores `v` where `mask` is true. | Element-aligned. Non-atomic. |
| `GatherIndex` | `GatherIndex(d, base, vi)`: `v[i] = base[vi[i]]`. | Element-aligned. **Extremely slow.** |
| `ScatterIndex`| `ScatterIndex(v, d, base, vi)`: `base[vi[i]] = v[i]`. | Element-aligned. **Extremely slow.** |
| `LoadInterleaved{2,3,4}` | De-interleaves data (e.g., `RGBRGB...` -> separate `R..`, `G..`, `B..` vectors). | Element-aligned. |
| `StoreInterleaved{2,3,4}`| Interleaves and stores data (e.g., separate `R,G,B` -> `RGBRGB...`). | Element-aligned. |

### 4.4. Arithmetic Operations

| Function | Description |
| :--- | :--- |
| `Add`, `Sub`, `Mul` | Lane-wise `+`, `-`, `*` (integer `Mul` is truncating). |
| `Div` | Lane-wise `/`. |
| `SaturatedAdd`, `SaturatedSub` | `+`/`-` with saturation for `u/i8`, `u/i16`. |
| `Abs`, `Neg` | Absolute value, negation. |
| `Min`, `Max` | Lane-wise minimum/maximum. |
| `MulAdd(a, b, c)` | Fused `a * b + c`. |
| `NegMulAdd(a, b, c)`| Fused `-a * b + c`. |
| `MulSub(a, b, c)` | Fused `a * b - c`. |
| `NegMulSub(a, b, c)`| Fused `-a * b - c`. |
| `Sqrt` | Square root. |
| `ApproximateReciprocal` | Fast, approximate `1.0 / x`. |
| `ApproximateReciprocalSqrt` | Fast, approximate `1.0 / sqrt(x)`. |
| `Round`, `Trunc`, `Ceil`, `Floor` | Floating-point rounding modes. |

### 4.5. Logical and Bitwise Operations

| Function | Description |
| :--- | :--- |
| `And`, `Or`, `Xor` | Lane-wise bitwise operations. |
| `Not` | Bitwise NOT (`~`). |
| `AndNot(a, b)` | Bitwise `(~a) & b`. |
| `ShiftLeft<N>`, `ShiftRight<N>` | Bit-shift by compile-time immediate `N`. |
| `Shl`, `Shr` | Bit-shift by a variable (vector) amount. |
| `RotateLeft<N>`, `RotateRight<N>` | Bit-rotation by immediate `N`. |
| `Rol`, `Ror` | Bit-rotation by a variable amount. |
| `PopulationCount` | Counts set bits in each lane. |
| `LeadingZeroCount`, `TrailingZeroCount` | Counts leading/trailing zeros. |
| `TestBit(v, bit)` | `(v & bit) == bit`. `bit` must have exactly one bit set. |
| `CopySign(magnitude, sign)` | Returns value with magnitude of `magnitude` and sign of `sign`. |

### 4.6. Comparisons and Masks

Comparisons return a `Mask<D>` object.

| Function | Description |
| :--- | :--- |
| `Eq(a, b)`, `Ne(a, b)` | Equal, Not Equal. |
| `Lt(a, b)`, `Le(a, b)` | Less Than, Less or Equal. |
| `Gt(a, b)`, `Ge(a, b)` | Greater Than, Greater or Equal. |
| `IsNaN(v)`, `IsInf(v)` | Checks for NaN or Infinity (float only). |
| `MaskFromVec(v)` | Converts a vector of all-0s/all-1s to a mask. |
| `VecFromMask(d, m)` | Converts a mask to a vector of all-0s/all-1s. |
| `Not(m)`, `And(m1, m2)`, `Or(m1, m2)`, `Xor(m1, m2)` | Logical operations on masks. |
| `AllTrue(d, m)`, `AllFalse(d, m)` | Returns `bool` indicating if all/no mask lanes are set. |
| `CountTrue(d, m)` | Returns `size_t` count of set lanes. Slower than `AllTrue`. |
| `FindFirstTrue(d, m)` | Returns `intptr_t` index of the first set lane, or -1. |
| `FirstN(d, n)` | Returns a mask with the first `n` lanes set. |
| `Compress(v, m)` | Gathers elements of `v` where `m` is true to the start of the vector. |
| `Expand(v, m)` | Scatters elements of `v` to positions where `m` is true, zeroing others. |

### 4.7. Control Flow and Predication

| Function | Signature | Description |
| :--- | :--- | :--- |
| `IfThenElse` | `V IfThenElse(M m, V yes, V no)` | `m[i] ? yes[i] : no[i]` |
| `IfThenElseZero`|`V IfThenElseZero(M m, V yes)` | `m[i] ? yes[i] : 0` |
| `IfThenZeroElse`|`V IfThenZeroElse(M m, V no)` | `m[i] ? 0 : no[i]` |
| `ZeroIfNegative`| `V ZeroIfNegative(V v)` | `v[i] < 0 ? 0 : v[i]` |

### 4.8. Type Conversions and Promotions

| Function | Description |
| :--- | :--- |
| `BitCast` | Reinterprets the bit pattern of a vector as a different type. No-cost. |
| `PromoteTo` | Widens lanes (e.g., `u8` -> `u16`). Takes a half-vector as input. |
| `PromoteLowerTo`, `PromoteUpperTo` | Widens the lower/upper half of a full vector. |
| `PromoteEvenTo`, `PromoteOddTo` | Widens the even/odd lanes of a full vector. |
| `DemoteTo` | Narrows lanes (e.g., `f32` -> `f16`), with saturation for integers. |
| `OrderedDemote2To` | Narrows two wide vectors into one concatenated narrow vector. |
| `TruncateTo` | Narrows lanes by discarding high bits (no saturation). |

### 4.9. Swizzle, Permute, and Shuffle Operations

These operations rearrange lanes. Their performance varies significantly by platform.

| Function | Description | Scope |
| :--- | :--- | :--- |
| `Combine(d, hi, lo)` | Concatenates two half-vectors `hi` and `lo` into a full vector. | Full Vector |
| `LowerHalf(v)`, `UpperHalf(d, v)` | Extracts the lower/upper half of a vector. | Full Vector |
| `Reverse` | Reverses all lanes. | Full Vector |
| `Reverse2`, `Reverse4`, `Reverse8` | Reverses lanes within groups of 2, 4, or 8. | Full Vector |
| `InterleaveLower/Upper` | Interleaves lanes from the lower/upper halves of two vectors. | 128-bit blocks |
| `ZipLower/Upper` | Same as `Interleave`, but returns a vector with wider lanes. | 128-bit blocks |
| `OddEven(odd, even)` | Creates a vector with lanes from `odd` at odd indices and `even` at even indices. | 128-bit blocks |
| `TableLookupBytes(tbl, idx)` | Byte-level shuffle using `idx` as indices into `tbl`. | 128-bit blocks |
| `TableLookupLanes(tbl, idx)`| Lane-level shuffle. **Slower** as it can cross blocks. | Full Vector |
| `Shuffle01`, `Shuffle1032`, etc. | Various fixed-pattern shuffles. | 128-bit blocks |

### 4.10. Reductions (Horizontal Operations)

**Warning**: These are significantly slower than vertical (lane-wise) operations.

| Function | Description |
| :--- | :--- |
| `SumOfLanes(d, v)` | Returns a vector where each lane contains the sum of all lanes in `v`. |
| `MinOfLanes(d, v)`, `MaxOfLanes(d, v)` | Returns a vector where each lane contains the min/max of all lanes in `v`. |
| `ReduceSum(d, v)`, `ReduceMin(d, v)`, `ReduceMax(d, v)` | Returns the scalar result of the reduction. Often more efficient than the `*OfLanes` variants. |

### 4.11. Specialized and Cryptographic Operations

| Function | Description | Availability |
| :--- | :--- | :--- |
| `AESRound`, `AESLastRound`, `AESRoundInv` | Single rounds of AES encryption/decryption. | Requires AES-NI on x86, Crypto extensions on Arm, etc. |
| `CLMulLower`, `CLMulUpper` | Carry-less multiplication. | Requires CLMUL instructions. |
| `SHA1*`, `SHA256*` | Hashing primitives. | Requires SHA extensions. |
| `CRC32C` | CRC32-C checksum. | Requires CRC32 instructions. |

---

## 5. The `contrib` Ecosystem: High-Level Modules

Highway provides several high-level, performance-optimized modules in its `contrib` directory.

| Module | Description | Key Functions/Classes |
| :--- | :--- | :--- |
| **`sort`** | **Vectorized Quicksort (VQSort)**. State-of-the-art, often outperforming `std::sort`, `ips4o`, and other SIMD sorts. Uses a novel 2D sorting network for base cases. | `VQSort`, `VQPartialSort`, `VQSelect` |
| **`thread_pool`** | A high-performance, NUMA-aware, auto-tuning thread pool designed for fine-grained parallel tasks. | `ThreadPool`, `Run` |
| **`matvec`** | SIMD-accelerated matrix-vector multiplication, optimized for various data types including `bfloat16_t`. | `MatVec`, `MatVecAdd` |
| **`bit_pack`** | Utilities for packing multiple small integers into a single larger one and vice-versa. | `Pack8`, `Pack16`, `Pack32`, `Pack64` |
| **`random`** | SIMD-accelerated Xoshiro pseudo-random number generator for parallel random number generation. | `VectorXoshiro`, `CachedXoshiro` |
| **`image`** | Classes for managing 2D image data with correct alignment and padding for SIMD operations. | `Image`, `Image3` |
| **`unroller`** | A powerful templated utility for automating loop unrolling, pipelining, and tail handling for SIMD code, which compilers often fail to do. | `Unroller`, `UnrollerUnit` |
| **`dot`** | Optimized dot-product implementation. | `Dot::Compute` |
| **`algo`** | Implementations of STL-like algorithms (`Find`, `CopyIf`, `Transform`) that are SIMD-aware and handle remainders safely. | `Find`, `FindIf`, `Copy`, `CopyIf`, `Transform`, `Generate` |

---

## 6. Performance, Pitfalls, and Best Practices

### 6.1. Do's and Don'ts: A Quick Reference

| Topic | Do | Don't |
| :--- | :--- | :--- |
| **Operators** | Use named functions (`hn::Add`, `hn::Lt`). | Use C++ operators (`+`, `<`) which are not portable to SVE/RVV. |
| **Memory** | Use `hwy::AllocateAligned` and `Load`. | Assume `std::vector` is sufficiently aligned. |
| **Data Types** | Use `auto`, `Vec<D>`, `Mask<D>`. | Use concrete types (`Vec128`, `__m256i`). |
| **Class Members**| Store arrays of lane type `T`. | Store vectors (`V`) as class members. |
| **Loops** | Increment pointers to `T` by `Lanes(d)`. | Use pointer arithmetic on vector pointers. |
| **Remainders** | Use `LoadN`/`StoreN` or `contrib/algo`. | Use `MaskedLoad` on unpadded memory (unsafe with ASAN). |
| **Performance** | Prefer vertical ops. Structure data as SoA. | Rely on horizontal ops (`Reduce*`) or shuffles in hot loops. Use AoS. |
| **Constants** | Use `Set(d, value)`. | Expect scalar-to-vector conversions to be implicit. |

### 6.2. Understanding Floating-Point Behavior

*   **FMA Precision**: `MulAdd` uses a single rounding step, which can produce slightly different (and often more accurate) results than separate `Mul` and `Add`. When writing tests, use a tolerance (e.g., a few ULPs) for comparisons. `HWY_NATIVE_FMA` macro can detect if hardware FMA is used.
*   **NaN Propagation**: The behavior of `Min` and `Max` with NaN inputs can vary across architectures, especially older ones. Use `MinNumber` and `MaxNumber` for IEEE 754-2019 compliant behavior where available.

### 6.3. AVX Throttling and Startup Costs: A Deep Dive

*   **The Issue**: Older Intel CPUs (pre-Ice Lake, especially Skylake) would reduce their clock frequency ("downclock") when executing "heavy" 256-bit or 512-bit instructions. This was done to manage power and thermal limits.
*   **Startup Penalty**: There is also a micro-architectural "startup cost" of 8-20µs when transitioning from a non-AVX state to using heavy AVX-512 instructions. During this time, instruction throughput is reduced.
*   **Practical Implications**:
    1.  **Not a Major Concern on Modern CPUs**: Ice Lake, Rocket Lake, and AMD Zen 4 have eliminated or drastically reduced this issue.
    2.  **Amortization is Key**: For large workloads (e.g., sorting >100 KiB), the massive speedup from 512-bit vectors far outweighs the one-time startup cost and any minor downclocking.
    3.  **Hysteresis**: The CPU remains in the "AVX-512 ready" state for hundreds of microseconds after the last AVX-512 instruction. If your application uses SIMD frequently, the startup cost is paid only once.
    4.  **The "Beggar Thy Neighbor" Effect**: If a small VQSort call triggers the startup penalty, subsequent *scalar* code may run slower for a few microseconds. This is a system-level optimization problem, best solved by vectorizing more code, not by avoiding AVX-512.

### 6.4. Debugging and Verification

*   **`hwy/print-inl.h`**: Use `hn::Print(d, "My Vector", v);` to print vector contents to `stderr`.
*   **Scalar Fallback**: Compile your code for the `HWY_SCALAR` or `HWY_EMU128` targets to run a pure C++ implementation. This is invaluable for isolating bugs in your SIMD logic versus the scalar reference.
*   **Testing Infrastructure**: Highway's test utilities (`HWY_EXPORT_AND_TEST_P`) automatically run tests for every available target on the host CPU, ensuring your code is correct across instruction sets.

---

## 7. Case Study: `orderbook_simd_codec`

This example demonstrates a practical application of Highway for compressing financial order book data. The core idea is to increase data compressibility before passing it to a generic compressor like zstd.

### 7.1. Algorithm Overview

1.  **Delta Encoding**: Consecutive snapshots of the order book are often similar. Instead of storing the full values, we store the difference (delta) from the previous snapshot. A bitwise `XOR` is used for this, as it's efficient and perfectly captures small floating-point changes.
2.  **Data Shuffling**: A 32-bit float or 16-bit float consists of multiple bytes (exponent, mantissa). These bytes have different statistical properties. Shuffling separates these bytes into different "planes". For example, all the least significant bytes from all numbers are grouped together. This improves the locality of similar-entropy data, making it much easier for a compression algorithm to find patterns.

### 7.2. `encode16`: Demotion, XOR, and Shuffle

This function encodes `float` snapshots into a compressed byte stream using `float16_t` as an intermediate format to save bandwidth.

1.  **Looping and Loading**: The code iterates through snapshots. For each, it loads the current and previous snapshot data into `float` vectors.
    ```cpp
    const VF32 v_curr_f32_lo = hn::LoadU(d32, current + i);
    // ... load hi, prev_lo, prev_hi
    ```
2.  **Demotion**: The `float` vectors are demoted to `float16_t` to reduce data size. This is a lossy conversion but acceptable for many use cases. `DemoteTo` is used on half-vectors.
    ```cpp
    auto v_curr_f16_lo = hn::DemoteTo(d16_half, v_curr_f32_lo);
    auto v_curr_f16_hi = hn::DemoteTo(d16_half, v_curr_f32_hi);
    ```
3.  **Combine and BitCast**: The two `float16_t` half-vectors are combined into a full vector. Then, `BitCast` reinterprets them as `uint16_t` vectors to perform the bitwise XOR delta.
    ```cpp
    VF16 v_curr_f16 = hn::Combine(d16, v_curr_f16_hi, v_curr_f16_lo);
    VU16 v_curr_u16 = hn::BitCast(du16, v_curr_f16);
    ```
4.  **XOR Delta**: The core delta-encoding step.
    ```cpp
    VU16 v_xor_u16 = hn::Xor(v_curr_u16, v_prev_u16);
    ```
5.  **Shuffle (`ShuffleFloat16`)**: This is the key to compressibility. It de-interleaves the bytes of the `float16_t` deltas.
    *   It loads `uint16_t` vectors.
    *   It uses `And` with a mask (`0x00FF`) to isolate the low bytes and `ShiftRight<8>` to get the high bytes.
    *   `OrderedTruncate2To` packs these bytes into `uint8_t` vectors.
    *   The final result is two separate arrays: one with all the low bytes, one with all the high bytes.

### 7.3. `decode16`: Unshuffling, Reconstruction, and Promotion

This reverses the process.

1.  **Unshuffle (`UnshuffleAndReconstruct`)**:
    *   Loads the separated low and high byte planes.
    *   Uses `ZipLower` and `ZipUpper` to interleave them back into `uint16_t` vectors. `Combine` puts the halves together. This reconstructs the `v_delta_u16` from the encode step.
2.  **Reconstruction (XOR)**:
    *   Loads the previous snapshot's state (as `float16_t`).
    *   `Xor`s the delta with the previous state to get the current state's `uint16_t` representation.
    *   `BitCast`s back to `float16_t`.
3.  **Promotion and Store**:
    *   Uses `PromoteLowerTo` and `PromoteUpperTo` to convert the `float16_t` vector back to two `float` vectors.
    *   Stores the final reconstructed `float` data.
4.  **State Update**: The newly reconstructed snapshot becomes the `prev_snapshot` for the next iteration.

### 7.4. The `float32` Path

The `encode32` and `decode32` functions follow the same logic but omit the `DemoteTo` and `PromoteTo` steps. The shuffle/unshuffle logic is more complex, separating the four bytes of each `float` into four distinct planes. This path is used when full precision is required and the increased data size is acceptable.

---

## 8. Instruction Portability Matrix

The following table, derived from `instruction_matrix.pdf`, shows the number of instructions required to implement each Highway operation on different platforms. A "1" indicates native, single-instruction support. Higher numbers imply emulation, which is slower. The **"Intersection"** column shows the number of instructions on the *best-supported* platform, giving an idea of the operation's ideal performance. A "1" in this column indicates a highly portable and efficient operation.

*Note on AVX1*: The table below starts with SSE4/AVX2. AVX1 primarily introduced 256-bit floating-point instructions. Most integer operations remained 128-bit until AVX2. Therefore, for integer code, AVX1 performance is generally similar to SSE4.

| Operation + Types | x86 SSE4, AVX2 | ppc (POWER8/9) w/ VSX | arm64 NEON | Intersection |
| :--- | :--- | :--- | :--- | :--- |
| **ARITH** | | | | |
| `add`, `sub` | 1 | 1 | 1 | 1 |
| `pairwise add` | 1 (f32/64), 9 | 1 (f32/64), 9 | 1 (i16/32/f32), 9 | 1 |
| `saturated_add` | 1 (u/i8/16), 9 | 1 (u/i8/16), 9 | 1 (all u/i), 9 | 1 |
| `average` | 1 (u8/16), 2 | 1 (u8/16), 2 | 1 (u8/16), 2 | 1 |
| `shift left/right by const`| 1 (i16/32/64), 2 | 1 | 1 | 1 |
| `bit-shift-right-var` | 1 (i32), 9 | 1 (i32), 9 | 1 (all u/i), 2 | 1 |
| `bit-shift-left-var` | 1 (i32), 9 | 1 (all), 1 | 1 (all u/i), 1 | 1 |
| `abs` | 1 (i8/16/32), 3 | 1 (i/f), 3 | 1 (i/f), 9 | 1 |
| `negate` | 1 (f), 2 | 1 (i/f), 2 | 1 (i/f), 1 | 1 |
| `min/max` | 1 (most), 2, 3 | 1 | 1 | 1 |
| `mul_truncate` (mullo) | 1 (i16/32), 9 | 1 | 1 | 1 |
| `mul_even` | 9 | 1 (i16/32), 2 | 1 (i/u), 9 | 1 |
| `muladd_fp` | 1 | 1 | 1 | 1 |
| **LOGICAL** | | | | |
| `and`, `or`, `xor` | 1 | 1 | 1 | 1 |
| `and(not a, b)` | 1 | 1 | 1 | 1 |
| `bitwise NOT` | 2 | 2 | 2 | 2 |
| `movmskb` | 1 | 5 | 5 | 1 |
| **LOAD/STORE** | | | | |
| `load/store_aligned` | 1 | 1 | 1 | 1 |
| `load/store_unaligned`| 1 | 1 | 1 | 1 |
| `load1_and_broadcast`| 1 (f32/i32), 2 | 1 (i16/32/f32), 2 | 1 | 1 |
| `stream` | 1 | 1 | 1 | 1 |
| **SWIZZLE** | | | | |
| `shift128 left/right` | 1 | 1 | 1 | 1 |
| `broadcast any lane`| 1 (f32/i32), 9 | 1 | 1 | 1 |
| `16-byte shuffle` | 1 | 1 | 1 | 1 |
| `Interleave/zip=unpack` | 1 | 1 | 1 | 1 |
| **CONVERSION** | | | | |
| `Expand to 2x width` | 1 | 1 | 1 | 1 |
| `Reducing to half width`| 1 | 1 | 1 | 1 |
| `Convert integer <=> real` | 1 (f32), 9 | 1 | 1 | 1 |
| **CRYPTO/HASH** | | | | |
| `SHA1`, `SHA256` | 1 | 1 | 1 | 1 |
| `AES` | 1 | 1 | 1 | 1 |
| `CRC32C` | 1 | 3 | 1 | 1 |
| `CLMUL` | 1 | 3 | 1 | 1 |
| **EMULATED** | | | | |
| `mulhi16` | 1 | 1 | 3 | 1 |
| `horz_sum` | 3..5 | 1..3 | 2 | 1 |

**Analysis and Takeaways**:

*   **Core Arithmetic is Cheap**: `add`, `sub`, `mul`, `and`, `or`, `xor` are universally supported with a single instruction.
*   **Shifts are Tricky**: Variable shifts (`Shr`, `Shl`) are not universally fast, especially on older x86 targets. Constant shifts (`ShiftLeft<N>`) are always preferred.
*   **Conversions Can Be Costly**: While widening/narrowing is often a single instruction, converting between integer and float can sometimes be emulated (e.g., `i64 <=> f64` on SSE4), as indicated by a cost of `9`.
*   **Horizontal Ops are Slow**: `pairwise add`, `horz_sum`, and `movmskb` show higher instruction counts on some platforms, confirming that cross-lane operations are less efficient than vertical ones.
*   **Specialized Ops are a Win (When Available)**: Crypto and hash instructions (`AES`, `SHA`, `CRC32C`, `CLMUL`) are single instructions where supported, but require emulation (higher cost) where not. Highway handles this abstraction.

---

## 9. Advanced Topics and Internals

### 9.1. Preprocessor Macros and Configuration

Highway's behavior can be fine-tuned with several macros, typically defined on the command line.

| Macro | Effect |
| :--- | :--- |
| `HWY_IS_TEST` | Compiles all attainable targets, not just the best baseline. Used for library self-tests. |
| `HWY_COMPILE_ONLY_STATIC` | Disables dynamic dispatch entirely, compiling only for the static target. |
| `HWY_COMPILE_ONLY_SCALAR` | Compiles only the pure C++, single-lane `HWY_SCALAR` target. |
| `HWY_DISABLED_TARGETS` | A bitmask of `HWY_Target` values to exclude from compilation. |
| `HWY_HAVE_INTEGER64`, `HWY_HAVE_FLOAT16/64` | `1` or `0`, indicating if the current target supports these types. |
| `HWY_HAVE_SCALABLE` | `1` if the target (SVE/RVV) has sizeless vectors. |

### 9.2. Extending Highway: Adding New Operations

To add a new operation to Highway:

1.  **Document it**: Add the function signature and description to `g3doc/quick_reference.md`.
2.  **Implement for each backend**: Add implementations to each of the `hwy/ops/*-inl.h` files (`x86_128-inl.h`, `arm_neon-inl.h`, etc.).
    *   If the operation can be implemented generically using other Highway ops, add it to `hwy/ops/generic_ops-inl.h` as a fallback.
    *   Use the internal macro systems (e.g., `HWY_SVE_FOREACH*`) to reduce repetition if the op is orthogonal across many types.
3.  **Add a Test**: Add a new test case to the appropriate `hwy/tests/*_test.cc` file. This involves creating a functor, a test runner function, and invoking it via `HWY_EXPORT_AND_TEST_P`.

### 9.3. Type Traits and Metaprogramming Helpers

Highway provides a suite of type traits in `hwy/base.h` for generic programming.

| Trait | Description |
| :--- | :--- |
| `MakeUnsigned<T>`, `MakeSigned<T>`, `MakeFloat<T>` | Returns a type of the same size but different category. |
| `MakeWide<T>`, `MakeNarrow<T>` | Returns a type of double/half the size. |
| `IsFloat<T>()`, `IsSigned<T>()` | Compile-time checks for type category. |
| `LimitsMin<T>()`, `LimitsMax<T>()` | Smallest/largest representable values. |
| `HWY_IF_*_D(D)` | SFINAE helpers for enabling templates based on tag type properties (e.g., `HWY_IF_FLOAT_D`). |