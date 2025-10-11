# Design philosophy

*   Performance is important but not the sole consideration. Anyone who goes to
    the trouble of using SIMD clearly cares about speed. However, portability,
    maintainability and readability also matter, otherwise we would write in
    assembly. We aim for performance within 10-20% of a hand-written assembly
    implementation on the development platform. There is no performance gap vs.
    intrinsics: Highway code can do anything they can. If necessary, you can use
    platform-specific instructions inside `#if HWY_TARGET == HWY_NEON` etc.

*   The guiding principles of C++ are "pay only for what you use" and "leave no
    room for a lower-level language below C++". We apply these by defining a
    SIMD API that ensures operation costs are visible, predictable and minimal.

*   Performance portability is important, i.e. the API should be efficient on
    all target platforms. Unfortunately, common idioms for one platform can be
    inefficient on others. For example: summing lanes horizontally versus
    shuffling. Documenting which operations are expensive does not prevent their
    use, as evidenced by widespread use of `HADDPS`. Performance acceptance
    tests may detect large regressions, but do not help choose the approach
    during initial development. Analysis tools can warn about some potential
    inefficiencies, but likely not all. We instead provide [a carefully chosen
    set of vector types and operations that are efficient on all target
    platforms](g3doc/instruction_matrix.pdf) (Armv8, PPC8, x86).

*   Future SIMD hardware features are difficult to predict. For example, AVX2
    came with surprising semantics (almost no interaction between 128-bit
    blocks) and AVX-512 added two kinds of predicates (writemask and zeromask).
    To ensure the API reflects hardware realities, we suggest a flexible
    approach that adds new operations as they become commonly available, with
    fallback implementations where necessary.

*   Masking/predication differs between platforms, and it is not clear how
    important the use cases are beyond the ternary operator `IfThenElse`.
    AVX-512/Arm SVE zeromasks are useful, but not supported by P0214R5. We
    provide `IfThen[Zero]Else[Zero]` variants.

*   "Width-agnostic" SIMD is more future-proof than user-specified fixed sizes.
    For example, valarray-like code can iterate over a 1D array with a
    library-specified vector width. This will result in better code when vector
    sizes increase, and matches the direction taken by
    [Arm SVE](https://alastairreid.github.io/papers/sve-ieee-micro-2017.pdf) and
    RiscV V as well as Agner Fog's
    [ForwardCom instruction set proposal](https://www.agner.org/optimize/forwardcom.pdf).
    However, some applications may require fixed sizes, so we also guarantee
    support for <= 128-bit vectors in each instruction set.

*   The API and its implementation should be usable and efficient with commonly
    used compilers, including MSVC. For example, we write `ShiftLeft<3>(v)`
    instead of `v << 3` because MSVC 2017 (aarch64) does not propagate the
    literal (https://godbolt.org/g/rKx5Ga). Highway requires function-specific
    target attributes, supported by GCC 4.9 / Clang 3.9 / MSVC 2015.

*   Efficient and safe runtime dispatch is important. Modules such as image or
    video codecs are typically embedded into larger applications such as
    browsers, so they cannot require separate binaries for each CPU. Libraries
    also cannot predict whether the application already uses AVX2 (and pays the
    frequency throttling cost), so this decision must be left to the
    application. Using only the lowest-common denominator instructions
    sacrifices too much performance. Therefore, we provide code paths for
    multiple instruction sets and choose the most suitable at runtime. To reduce
    overhead, dispatch should be hoisted to higher layers instead of checking
    inside every low-level function. Highway supports inlining functions in the
    same file or in `*-inl.h` headers. We generate all code paths from the same
    source to reduce implementation- and debugging cost.

*   Not every CPU need be supported. To reduce code size and compile time, we
    group x86 targets into clusters. In particular, SSE3 instructions are only
    used/available if S-SSE3 is also available, and AVX only if AVX2 is also
    supported.

*   Access to platform-specific intrinsics is necessary for acceptance in
    performance-critical projects. We provide conversions to and from intrinsics
    to allow utilizing specialized platform-specific functionality, and simplify
    incremental porting of existing code.

*   The core API should be compact and easy to learn; we provide a
    [concise reference](quick_reference.md).

## Prior API designs

The author has been writing SIMD code since 2002: first via assembly language,
then intrinsics, later Intel's `F32vec4` wrapper, followed by three generations
of custom vector classes. The first used macros to generate the classes, which
reduces duplication but also readability. The second used templates instead.
The third (used in highwayhash and PIK) added support for AVX2 and runtime
dispatch. The current design (used in JPEG XL) enables code generation for
multiple platforms and/or instruction sets from the same source, and improves
runtime dispatch.

## Overloaded function API

Most C++ vector APIs rely on class templates. However, the Arm SVE vector type
is sizeless and cannot be wrapped in a class. We instead rely on overloaded
functions. Overloading based on vector types is also undesirable because SVE
vectors cannot be default-constructed. We instead use a dedicated tag type
`Simd` for overloading, abbreviated to `D` for template arguments and `d` in
lvalues.

Note that generic function templates are possible (see generic_ops-inl.h).

## Masks

AVX-512 introduced a major change to the SIMD interface: special mask registers
(one bit per lane) that serve as predicates. It would be expensive to force
AVX-512 implementations to conform to the prior model of full vectors with lanes
set to all one or all zero bits. We instead provide a Mask type that emulates
a subset of this functionality on other platforms at zero cost.

Masks are returned by comparisons and `TestBit`; they serve as the input to
`IfThen*`. We provide conversions between masks and vector lanes. On targets
without dedicated mask registers, we use FF..FF as the definition of true. To
also benefit from x86 instructions that only require the sign bit of
floating-point inputs to be set, we provide a special `ZeroIfNegative` function.

## Differences vs. [P0214R5](https://goo.gl/zKW4SA) / std::experimental::simd

1.  Allowing the use of built-in vector types by relying on non-member
    functions. By contrast, P0214R5 requires a wrapper class, which does not
    work for sizeless vector types currently used by Arm SVE and Risc-V.

1.  Supporting many more operations such as 128-bit compare/minmax, AES/CLMUL,
    `AndNot`, `AverageRound`, bit-shift by immediates, compress/expand,
    fixed-point mul, `IfThenElse`, interleaved load/store, lzcnt, mask find/set,
    masked load/store, popcount, reductions, saturated add/sub, scatter/gather.

1.  Designing the API to avoid or minimize overhead on AVX2/AVX-512 caused by
    crossing 128-bit 'block' boundaries.

1.  Avoiding the need for non-native vectors. By contrast, P0214R5's `simd_cast`
    returns `fixed_size<>` vectors which are more expensive to access because
    they reside on the stack. We can avoid this plus additional overhead on
    Arm/AVX2 by defining width-expanding operations as functions of a vector
    part, e.g. promoting half a vector of `uint8_t` lanes to one full vector of
    `uint16_t`, or demoting full vectors to half vectors with half-width lanes.

1.  Guaranteeing access to the underlying intrinsic vector type. This ensures
    all platform-specific capabilities can be used. P0214R5 instead only
    'encourages' implementations to provide access.

1.  Enabling safe runtime dispatch and inlining in the same binary. P0214R5 is
    based on the Vc library, which does not provide assistance for linking
    multiple instruction sets into the same binary. The Vc documentation
    suggests compiling separate executables for each instruction set or using
    GCC's ifunc (indirect functions). The latter is compiler-specific and risks
    crashes due to ODR violations when compiling the same function with
    different compiler flags. We solve this problem via target-specific
    namespaces and attributes (see HOWTO section below). We also permit a mix of
    static target selection and runtime dispatch for hotspots that may benefit
    from newer instruction sets if available.

1.  Omitting inefficient or non-performance-portable operations such as `hmax`,
    `operator[]`, and unsupported integer comparisons. Applications can often
    replace these operations at lower cost than emulating that exact behavior.

1.  Omitting `long double` types: these are not commonly available in hardware.

1.  Ensuring signed integer overflow has well-defined semantics (wraparound).

1.  Avoiding hidden performance costs. P0214R5 allows implicit conversions from
    integer to float, which costs 3-4 cycles on x86. We make these conversions
    explicit to ensure their cost is visible.

## Other related work

*   [Neat SIMD](http://ieeexplore.ieee.org/stamp/stamp.jsp?arnumber=7568423)
    adopts a similar approach with interchangeable vector/scalar types and
    a compact interface. It allows access to the underlying intrinsics, but
    does not appear to be designed for other platforms than x86.

*   UME::SIMD ([code](https://goo.gl/yPeVZx), [paper](https://goo.gl/2xpZrk))
    also adopts an explicit vectorization model with vector classes.
    However, it exposes the union of all platform capabilities, which makes the
    API harder to learn (209-page spec) and implement (the estimated LOC count
    is [500K](https://goo.gl/1THFRi)). The API is less performance-portable
    because it allows applications to use operations that are inefficient on
    other platforms.

*   Inastemp ([code](https://goo.gl/hg3USM), [paper](https://goo.gl/YcTU7S))
    is a vector library for scientific computing with some innovative features:
    automatic FLOPS counting, and "if/else branches" using lambda functions.
    It supports IBM Power8, but only provides float and double types and does
    not support SVE without assuming the runtime vector size.


---

# Frequently Asked Questions

## Getting started

Q0.0: How do I **get the Highway library**?

A: Highway is available in numerous package managers, e.g. under the name
libhwy-dev. After installing, you can add it to your CMake-based build via
`find_package(HWY 1.3.0)` and `target_link_libraries(your_project PRIVATE hwy)`.

Alternatively, if using Git for version control, you can use Highway as a
'submodule' by adding the following to .gitmodules:

```
[submodule "highway"]
    path = highway
    url = https://github.com/google/highway
```

Then, anyone who runs `git clone --recursive` on your repository will also get
Highway. If not using Git, you can also manually download the
[Highway code](https://github.com/google/highway/releases) and add it to your
source tree.

For building Highway yourself, the two best-supported build systems are CMake
and Bazel. For the former, insert `add_subdirectory(highway)` into your
CMakeLists.txt. For the latter, we provide a BUILD file and your project can
reference it by adding `deps = ["//path/highway:hwy"]`.

If you use another build system, add `hwy/per_target.cc` and `hwy/targets.cc` to
your list of files to compile and link. As of writing, all other files are
headers typically included via highway.h.

If you are interested in a single-header version of Highway, please raise an
issue so we can understand your use-case.

Q0.1: What's the **easiest way to start using Highway**?

A: Copy an existing file such as `hwy/examples/benchmark.cc` or `skeleton.cc` or
another source already using Highway.
This ensures that the 'boilerplate' (namespaces, include order) are correct.

Then, in the function `RunBenchmarks` (for benchmark.cc) or `FloorLog2` (for
skeleton.cc), you can insert your own code. For starters it can be written
entirely in normal C++. This can still be beneficial because your code will be
compiled with the appropriate flags for SIMD, which may allow the compiler to
autovectorize your C++ code especially if it is straightforward integer code
without conditional statements/branches.

Next, you can wrap your code in `#if HWY_TARGET == HWY_SCALAR || HWY_TARGET ==
HWY_EMU128` and into the `#else` branch, put a vectorized version of your code
using the Highway intrinsics (see Documentation section below). If you also
create a test by copying one of the source files in `hwy/tests/`, the Highway
infrastructure will run your test for all supported targets. Because one of the
targets SCALAR or EMU128 are always supported, this will ensure that your vector
code behaves the same as your original code.

## Documentation

Q1.1: How do I **find the Highway op name** corresponding to an existing
intrinsic?

A: Search for the intrinsic in (for example)
[x86_128-inl.h](https://github.com/google/highway/blob/master/hwy/ops/x86_128-inl.h).
The Highway op is typically the name of the function that calls the intrinsic.
See also the
[quick reference](https://github.com/google/highway/blob/master/g3doc/quick_reference.md)
which lists all of the Highway ops.

Q1.2: Are there **examples of porting intrinsics to Highway**?

A: See https://github.com/google/highway#examples.

Q1.3: Where do I find documentation for each **platform's intrinsics**?

A: See Intel [guide](https://www.intel.com/content/www/us/en/docs/intrinsics-guide),
Arm [guide](https://developer.arm.com/architectures/instruction-sets/intrinsics)
and [SVE](https://dougallj.github.io/asil/),
RISC-V V [spec](https://github.com/riscv/riscv-v-spec/blob/master/v-spec.adoc)
and [guide](https://dzaima.github.io/intrinsics-viewer/#),
[WebAssembly](https://nemequ.github.io/waspr/intrinsics), PPC
[ISA](https://openpowerfoundation.org/specifications/isa/) and
[intrinsics](https://openpowerfoundation.org/specifications/vectorintrinsicprogrammingreference/).

Q1.4: Where do I find **instruction latency/throughput**?

A: For x86, a combination of [uops.info](https://www.uops.info/table.html) and
https://agner.org/optimize/, plus Intel's above intrinsics guide and
[AMD's sheet (zip file)](https://www.amd.com/system/files/TechDocs/56665.zip).
For Arm, the
[Software_Optimization_Guide](https://developer.arm.com/documentation/pjdoc466751330-9685/latest/)
for Neoverse V1 etc. For RISC-V, the vendor's tables (typically not publicly
available).

Q1.5: Where can I find **inspiration for SIMD-friendly algorithms**? A:

-   [Algorithms for Modern Hardware online book](https://en.algorithmica.org/hpc/)
-   [SIMD for C++ developers](http://const.me/articles/simd/simd.pdf)
-   [Bit twiddling collection](https://graphics.stanford.edu/~seander/bithacks.html)
-   [SIMD-within-a-register](http://aggregate.org/MAGIC/)
-   Hacker's Delight book, which has a huge collection of bitwise identities,
    but is written for hypothetical RISC CPUs, which differ in some ways from
    the SIMD capabilities of current CPUs.

Q1.6: How do I **predict performance**?

A: The best approach by far is end-to-end application benchmarking. Typical
microbenchmarks are subject to numerous pitfalls including unrealistic cache and
branch predictor hit rates (unless the benchmark randomizes its behavior). But
sometimes we would like a quick indication of whether a short piece of code runs
efficiently on a given CPU. Intel's IACA used to serve this purpose but has been
discontinued. We now recommend llvm-mca,
[integrated into Compiler Explorer](https://gcc.godbolt.org/z/n-KcQ-). This
shows the predicted throughput and the pressure on the various functional units,
but does not cover dynamic behavior including frontend and cache. For a bit more
detail, see
[https://en.algorithmica.org/hpc/profiling/mca/](https://en.algorithmica.org/hpc/profiling/mca/).
chriselrod mentioned the recently published [uica](https://uica.uops.info/),
which is reportedly more accurate
([paper](https://arxiv.org/pdf/2107.14210.pdf)).

## Correctness

Q2.1: **Which targets are covered** by my tests?

A: Tests execute for every target supported by the current CPU. The CPU may vary
across runs in a cloud environment, so you may want to specify constraints to
ensure the CPU is as recent as possible.

Q2.2: Why do **floating-point results differ** on some platforms?

A: It is commonly believed that floating-point reproducibility across platforms
is infeasible. That is somewhat pessimistic, but not entirely wrong. Although
IEEE-754 guarantees certain properties, including the rounding of each
operation, commonly used compiler flags can invalidate them. In particular,
clang/GCC -ffp-contract and MSVC /fp:contract can change results of anything
involving multiply followed by add. This is usually helpful (fusing both
operations into a single FMA, with only a single rounding), but depending on the
computation typically changes the end results by around 10^-5. Using Highway's
`MulAdd` op can have the same effect: SSE4, NEON and WASM may not support FMA,
but most other platforms do. A common workaround is to use a tolerance when
comparing expected values. For robustness across both large and small values, we
recommend both a relative and absolute (L1 norm) tolerance. The -ffast-math flag
can have more subtle and dangerous effects. It allows reordering operations
(which can also change results), but also removes guarantees about NaN, thus we
do not recommend using it.

Q2.3: How do I make my code **safe for asan and msan**?

A: The main challenge is dealing with the remainders in arrays not divisible by
the vector length. Using `LoadU`, or even `MaskedLoad` with the mask set to
`FirstN(d, remaining_lanes)`, may trigger page faults or asan errors. We instead
recommend using `hwy/contrib/algo/transform-inl.h`. Rather than having to write
a loop plus remainder handling, you simply define a templated (lambda) function
implementing one loop iteration. The `Generate` or `Transform*` functions then
take care of remainder handling.

## API design

Q3.1: Are the **`d` arguments optimized out**?

A: Yes, `d` is an lvalue of the zero-sized type `Simd<>`, typically obtained via
`ScalableTag<T>`. These only serve to select overloaded functions and do not
occupy any storage at runtime.

Q3.2: Why do **only some functions have a `d` argument**?

A: Ops which receive and return vectors typically do not require a `d` argument
because the type information on vectors (either built-in or wrappers) is
sufficient for C++ overload resolution. The `d` argument is required for:

```
-   Influencing the number of lanes loaded/stored from/to memory. The
    arguments to `Simd<>` include an upper bound `N`, and a shift count
    `kPow2` to divide the actual number of lanes by a power of two.
-   Indicating the desired vector or mask type to return from 'factory'
    functions such as `Set` or `FirstN`, `BitCast`, or conversions such as
    `PromoteTo`.
-   Disambiguating the argument type to ops such as `VecFromMask` or
    `AllTrue`, because masks may be generic types shared between multiple
    lane types.
-   Determining the actual number of lanes for certain ops, in particular
    those defined in terms of the upper half of a vector (`UpperHalf`, but
    also `Combine` or `ConcatUpperLower`) and reductions such as
    `MaxOfLanes`.
```

Q3.3: What's the policy for **adding new ops**?

A: Please reach out, we are happy to discuss via Github issue. The general
guideline is that there should be concrete plans to use the op, and it should be
efficiently implementable on all platforms without major performance cliffs. In
particular, each implementation should be at least as efficient as what is
achievable on any platform using portable code without the op. See also the
[wishlist for ops](op_wishlist.md).

Q3.4: `auto` is discouraged, **what vector type** should we use?

A: You can use `Vec<D>` or `Mask<D>`, where `D` is the type of `d` (in fact we
often use `decltype(d)` for that). To keep code short, you can define
typedefs/aliases, for example `using V = Vec<decltype(d)>`. Note that the
Highway implementation uses `VFromD<D>`, which is equivalent but currently
necessary because `Vec` is defined after the Highway implementations in
hwy/ops/*.

Q3.5: **Why is base.h separate** from highway.h?

A: It can be useful for files that just want compiler-dependent macros, for
example `HWY_RESTRICT` in public headers. This avoids the expense of including
the full `highway.h`, which can be large because some platform headers declare
thousands of intrinsics.

Q3.6: What are **restrict pointers** and when to use `HWY_RESTRICT`?

This relates to aliasing. If a function has two pointer arguments of the same
type, and perhaps also extern/static variables of that type, the compiler might
have to be very conservative about caching the variables or pointer accesses
because their value could change after writes to the pointer.

`float* HWY_RESTRICT p` means a pointer to float, and this pointer is the only
way to access the pointed-to object/array. In particular, this promises that `p`
does not alias other pointers. This usually improves codegen when there are
multiple pointers, at least one of which is const. Beware that the generated
code might not behave as expected if you break the promise.

## Portability

Q4.1: How do I **only generate code for a single instruction set** (static
dispatch)?

A: Suppose we know that all target CPUs support a given baseline (for example
SSE4). Then we can reduce binary size and compilation time by only generating
code for its instruction set. This is actually the default for Highway code that
does not use foreach_target.h. Highway detects via predefined macros which
instruction sets the compiler is allowed to use, which is governed by compiler
flags. This [example](https://gcc.godbolt.org/z/rGnjMevKG) documents which flags
are required on x86.

Q4.2: Why does my working x86 code **not compile on SVE or RISC-V**?

A: Assuming the code uses only documented identifiers (not, for example, the
AVX2-specific `Vec256`), the problem is likely due to compiler limitations
related to sizeless vectors. Code that works on x86 or NEON but not other
platforms is likely breaking one of the following rules:

-   Use functions (Eq, Lt) instead of overloaded operators (`==`, `<`);
-   Prefix Highway ops with `hwy::HWY_NAMESPACE`, or an alias (`hn::Load`) or
    ensure your code resides inside `namespace hwy::HWY_NAMESPACE`;
-   Avoid arrays of vectors and static/thread_local/member vectors; instead use
    arrays of the lane type (T).
-   Avoid pointer arithmetic on vectors; instead increment pointers to lanes by
    the vector length (`Lanes(d)`).

Q4.3: Why are **class members not allowed**?

A: This is a limitation of clang and GCC, which disallow sizeless types
(including SVE and RISC-V vectors) as members. This is because it is not known
at compile time how large the vectors are. MSVC does not yet support SVE nor
RISC-V V, so the issue has not yet come up there.

Q4.4: Why are **overloaded operators not allowed**?

A: C++ disallows overloading functions for built-in types, and vectors on some
platforms (SVE, RISC-V) are indeed built-in types precisely due to the above
limitation. Discussions are ongoing whether the compiler could add builtin
`operator<(unspecified_vector, unspecified_vector)`. When(if) that becomes
widely supported, this limitation can be lifted.

Q4.5: Can I declare **arrays of lanes on the stack**?

A: This mostly works, but is not necessarily safe nor portable. On RISC-V,
vectors can be quite large (64 KiB for LMUL=8), which can exceed the stack size.
It is better to use `hwy::AllocateAligned<T>(Lanes(d))`.

## Boilerplate

Q5.1: What is **boilerplate**?

A: We use this to refer to reusable infrastructure which mostly serves to
support runtime dispatch. We strongly recommend starting a SIMD project by
copying from an existing one, because the ordering of code matters and the
vector-specific boilerplate may be unfamiliar. See hwy/examples/skeleton.cc
and https://github.com/google/highway#examples.


Q5.2: What's the difference between **`HWY_BEFORE_NAMESPACE` and `HWY_ATTR`**?

A: Both are ways of enabling SIMD code generation in clang/gcc. The former is a
pragma that applies to all subsequent namespace-scope and member functions, but
not lambda functions. It can be more convenient than specifying `HWY_ATTR` for
every function. However, `HWY_ATTR` is still necessary for lambda functions that
use SIMD.

Q5.3: **Why use `HWY_NAMESPACE`**?

A: This is only required when using foreach_target.h to generate code for
multiple targets and dispatch to the best one at runtime. The namespace name
changes for each target to avoid ODR violations. This would not be necessary for
binaries built for a single target instruction set. However, we recommend
placing your code in a `HWY_NAMESPACE` namespace (nested under your project's
namespace) regardless so that it will be ready for runtime dispatch if you want
that later.

Q5.4: What are these **unusual include guards**?

A: Suppose you want to share vector code between several translation units, and
ensure it is inlined. With normal code we would use a header. However,
foreach_target.h wants to re-compile (via repeated preprocessor `#include`) a
translation unit once per target. A conventional include guard would strip out
the header contents after the first target. By convention, we use header files
named *-inl.h with a special include guard of the form:

```
#if defined(MYPROJECT_FILE_INL_H_TARGET) == defined(HWY_TARGET_TOGGLE)
#ifdef MYPROJECT_FILE_INL_H_TARGET
#undef MYPROJECT_FILE_INL_H_TARGET
#else
#define MYPROJECT_FILE_INL_H_TARGET
#endif
```

Highway takes care of defining and un-defining `HWY_TARGET_TOGGLE` after each
recompilation such that the guarded header is included exactly once per target.
Again, this effort is only necessary when using foreach_target.h. However, we
recommend using the special include guards already so your code is ready for
runtime dispatch.

Q5.5: How do I **prevent lint warnings for the include guard**?

A: The linter wishes to see a normal include guard at the start of the file. We
can simply insert an empty guard, followed by our per-target guard.

```
// Start of file: empty include guard to avoid lint errors
#ifndef MYPROJECT_FILE_INL_H_
#define MYPROJECT_FILE_INL_H_
#endif
// Followed by the actual per-target guard as above
```

## Efficiency

Q6.1: I heard that modern CPUs support unaligned loads efficiently. Why does
Highway **differentiate unaligned and aligned loads/stores**?

A: It is true that Intel CPUs since Haswell have greatly reduced the penalty for
unaligned loads. Indeed the `LDDQU` instruction intended to reduce their
performance penalty is no longer necessary because normal loads (`MOVDQU`) now
behave in the same way, splitting unaligned loads into two aligned loads.
However, this comes at a cost: using two (both) load ports per cycle. This can
slow down low-arithmetic-intensity algorithms such as dot products that mainly
load without performing much arithmetic. Also, unaligned stores are typically
more expensive on any platform. Thus we recommend using aligned stores where
possible, and testing your code on x86 (which may raise faults if your pointers
are actually unaligned). Note that the more specialized memory operations apart
from Load/Store (e.g. `CompressStore` or `BlendedStore`) are not specialized for
aligned pointers; this is to avoid doubling the number of memory ops.

Q6.2: **When does `Prefetch` help**?

A: Prefetching reduces apparent memory latency by starting the process of
loading from cache or DRAM before the data is actually required. In some cases,
this can be a 10-20% improvement if the application is indeed latency sensitive.
However, the CPU may already be triggering prefetches by analyzing your access
patterns. Depending on the platform, one or two separate instances of continuous
forward or backward scans are usually automatically detected. If so, then
additional prefetches may actually degrade performance. Also, applications will
not see much benefit if they are bottlenecked by something else such as vector
execution resources. Finally, a prefetch only helps if it comes sufficiently
before the subsequent load, but not so far ahead that it again falls out of the
cache. Thus prefetches are typically applied to future loop iterations.
Unfortunately, the prefetch distance (gap between current position and where we
want to prefetch) is highly platform- and microarchitecture dependent, so it can
be difficult to choose a value appropriate for all platforms.

Q6.3: Is **CPU clock throttling** really an issue?

A: Early Intel implementations of AVX2 and especially AVX-512 reduced their
clock frequency once certain instructions are executed. A
[microbenchmark](https://lemire.me/blog/2018/08/15/the-dangers-of-avx-512-throttling-a-3-impact/)
specifically designed to reveal the worst case (with only few AVX-512
instructions) shows a 3-4% slowdown on Skylake. Note that this is for a single
core; the effect depends on the number of cores using SIMD, and the CPU type
(Bronze/Silver are more heavily affected than Gold/Platinum). However, the
throttling is defined relative to an arbitrary base frequency; what actually
matters is the measured performance. Because throttling or SIMD usage can affect
the entire system, it is important to measure end-to-end application performance
rather than rely on microbenchmarks. In practice, we find the speedup from
sustained SIMD usage (not just sporadic instructions amid mostly scalar code) is
much larger than the impact of throttling. For JPEG XL image decompression, we
observe a 1.4-1.6x end to end speedup from AVX-512 vs. AVX2, even on multiple
cores of a Xeon Gold. For
[vectorized Quicksort](https://github.com/google/highway/blob/master/hwy/contrib/sort/README.md#study-of-avx-512-downclocking),
we find that throttling is not detectable on a single Skylake core, and the
AVX-512 startup overhead is worthwhile for inputs >= 80 KiB. Note that
throttling is
[no longer a concern on recent Intel](https://travisdowns.github.io/blog/2020/08/19/icl-avx512-freq.html#summary)
implementations of AVX-512 (Icelake and Rocket Lake client), and AMD CPUs do not
throttle AVX2 or AVX-512.

Q6.4: Why does my CPU sometimes only execute **one vector instruction per
cycle** even though the specs say it could do 2-4?

A: CPUs and fast food restaurants assume there will be a mix of
instructions/food types. If everyone orders only french fries, that unit will be
the bottleneck. Instructions such as permutes/swizzles and comparisons are
assumed to be less common, and thus can typically only execute one per cycle.
Check the platform's optimization guide for the per-instruction "throughput".
For example, Intel Skylake executes swizzles on port 5, and thus only one per
cycle. Similarly, Arm V1 can only execute one predicate-setting instruction
(including comparisons) per cycle. As a workaround, consider replacing equality
comparisons with the OR-sum of XOR differences.

Q6.5: How **expensive are Gather/Scatter**?

A: Platforms that support it typically process one lane per cycle. This can be
far slower than normal Load/Store (which can typically handle two or even three
entire *vectors* per cycle), so avoid them where possible. However, some
algorithms such as rANS entropy coding and hash tables require gathers, and it
is still usually better to use them than to avoid vectorization entirely.

## Troubleshooting

Q7.1: When building with clang-16, I see errors such as `DWARF error: invalid or
unhandled FORM value: 0x25` or `undefined reference to __extendhfsf2`.

A: This can happen if clang has been updated but compiler-rt has not. Action:
When installing Clang 16 from apt.llvm.org, ensure libclang-rt-16-dev is also
installed. This was caused by LLVM 16 changing the ABI of `__extendhfsf2` to
match the GCC ABI, which requires the entire toolchain to be updated. See #1709
for more information.

Q7.2: I see build errors mentioning `inlining failed in call to ‘always_inline’
‘hwy::PreventElision<int&>(int&)void’: target specific option mismatch`.

A: This is caused by a conflict between `-m` compiler flags and Highway's
dynamic dispatch mode, and is typically triggered by defining `HWY_IS_TEST` (set
by our CMake/Bazel builds for tests) or `HWY_COMPILE_ALL_ATTAINABLE`. See below
for a workaround; first some background. The goal of dynamic dispatch is to
compile multiple versions of the code, one per target. When `-m` compiler flags
are used to force a certain baseline, it can be that non-SIMD, forceinline
functions such as `PreventElision` are compiled for a newer CPU baseline than
the minimum target that Highway sets via `#pragma`. The compiler enforces a
safety check: inlining higher-baseline functions into a normal function raises
an error. This would not occur in most applications because Highway only enables
targets at or above the baseline set by `-m` flags. However, Highway's tests aim
to cover all targets by defining `HWY_IS_TEST`. When that or
`HWY_COMPILE_ALL_ATTAINABLE` are defined, then older targets are also compiled
and the incompatibility arises. One possible solution is to disable these modes
by defining `HWY_COMPILE_ONLY_STATIC`, which is checked first. Then, only the
baseline target is used and dynamic dispatch is effectively disabled. If you
still want to dispatch, but just avoid targets superseded by the baseline,
define `HWY_SKIP_NON_BEST_BASELINE`. A third option is to avoid `-m` flags
entirely, because they contradict the goals of test coverage and dynamic
dispatch, or only set the ones that correspond to the oldest target Highway
supports. See #1460, #1570, and #1707 for more information.

---

# Highway implementation details

## Introduction

This doc explains some of the Highway implementation details; understanding them
is mainly useful for extending the library. Bear in mind that Highway is a thin
wrapper over 'intrinsic functions' provided by the compiler.

## Vectors vs. tags

The key to understanding Highway is to differentiate between vectors and
zero-sized tag arguments. The former store actual data and are mapped by the
compiler to vector registers. The latter (`Simd<>` and `SizeTag<>`) are only
used to select among the various overloads of functions such as `Set`. This
allows Highway to use builtin vector types without a class wrapper.

Class wrappers are problematic for SVE and RVV because LLVM (or at least Clang)
does not allow member variables whose type is 'sizeless' (in particular,
built-in vectors). To our knowledge, Highway is the only C++ vector library that
supports SVE and RISC-V without compiler flags that indicate what the runtime
vector length will be. Such flags allow the compiler to convert the previously
sizeless vectors to known-size vector types, which can then be wrapped in
classes, but this only makes sense for use-cases where the exact hardware is
known and rarely changes (e.g. supercomputers). By contrast, Highway can run on
unknown hardware such as heterogeneous clouds or client devices without
requiring a recompile, nor multiple binaries.

Note that Highway does use class wrappers where possible, in particular NEON,
WASM and x86. The wrappers (e.g. Vec128) are in fact required on some platforms
(x86 and perhaps WASM) because Highway assumes the vector arguments passed e.g.
to `Add` provide sufficient type information to identify the appropriate
intrinsic. By contrast, x86's loosely typed `__m128i` built-in type could
actually refer to any integer lane type. Because some targets use wrappers and
others do not, incorrect user code may compile on some platforms but not others.
This is because passing class wrappers as arguments triggers argument-dependent
lookup, which would find the `Add` function even without namespace qualifiers
because it resides in the same namespace as the wrapper. Correct user code
qualifies each call to a Highway op, e.g. with a namespace alias `hn`, so
`hn::Add`. This works for both wrappers and built-in vector types.

## Adding a new target

Adding a target requires updating about ten locations: adding a macro constant
to identify it, hooking it into static and dynamic dispatch, detecting support
at runtime, and identifying the target name. The easiest and safest way to do
this is to search for one of the target identifiers such as `HWY_AVX3_DL`, and
add corresponding logic for your new target. Note the upper limits on the number
of targets per platform imposed by `HWY_MAX_DYNAMIC_TARGETS`.

## When to use -inl.h

By convention, files whose name ends with `-inl.h` contain vector code in the
form of inlined function templates. In order to support the multiple compilation
required for dynamic dispatch on platforms which provide several targets, such
files generally begin with a 'per-target include guard' of the form:

```
#if defined(HWY_PATH_NAME_INL_H_) == defined(HWY_TARGET_TOGGLE)
#ifdef HWY_PATH_NAME_INL_H_
#undef HWY_PATH_NAME_INL_H_
#else
#define HWY_PATH_NAME_INL_H_
#endif
// contents to include once per target
#endif  // HWY_PATH_NAME_INL_H_
```

This toggles the include guard between defined and undefined, which is
sufficient to 'reset' the include guard when beginning a new 'compilation pass'
for the next target. This is accomplished by simply re-#including the user's
translation unit, which may in turn `#include` one or more `-inl.h` files. As an
exception, `hwy/ops/*-inl.h` do not require include guards because they are all
included from highway.h, which takes care of this in a single location. Note
that platforms such as RISC-V which currently only offer a single target do not
require multiple compilation, but the same mechanism is used without actually
re-#including. For both of those platforms, it is possible that additional
targets will later be added, which means this mechanism will then be required.

Instead of a -inl.h file, you can also use a normal .cc/.h component, where the
vector code is hidden inside the .cc file, and the header only declares a normal
non-template function whose implementation does `HWY_DYNAMIC_DISPATCH` into the
vector code. For an example of this, see
[vqsort.cc](../hwy/contrib/sort/vqsort.cc).

Considerations for choosing between these alternatives are similar to those for
regular headers. Inlining and thus `-inl.h` makes sense for short functions, or
when the function must support many input types and is defined as a template.
Conversely, non-inline `.cc` files make sense when the function is very long
(such that call overhead does not matter), and/or is only required for a small
set of input types. [Math functions](../hwy/contrib/math/math-inl.h)
can fall into either case, hence we provide both inline functions and `Call*`
wrappers.

## Use of macros

Highway ops are implemented for up to 12 lane types, which can make for
considerable repetition - even more so for RISC-V, which can have seven times as
many variants (one per LMUL in `[1/8, 8]`). The various backends
(implementations of one or more targets) differ in their strategies for handling
this, in increasing order of macro complexity:

*   `x86_*` and `wasm_*` simply write out all the overloads, which is
    straightforward but results in 4K-6K line files.

*   [arm_sve-inl.h](../hwy/ops/arm_sve-inl.h) defines 'type list'
    macros `HWY_SVE_FOREACH*` to define all overloads for most ops in a single
    line. Such an approach makes sense because SVE ops are quite orthogonal
    (i.e. generally defined for all types and consistent).

*   [arm_neon-inl.h](../hwy/ops/arm_neon-inl.h) also uses type list
    macros, but with a more general 'function builder' which helps to define
    custom function templates required for 'unusual' ops such as `ShiftLeft`.

*   [rvv-inl.h](../hwy/ops/rvv-inl.h) has the most complex system
    because it deals with both type lists and LMUL, plus support for widening or
    narrowing operations. The type lists thus have additional arguments, and
    there are also additional lists for LMUL which can be extended or truncated.

## Code reuse across targets

The set of Highway ops is carefully chosen such that most of them map to a
single platform-specific intrinsic. However, there are some important functions
such as `AESRound` which may require emulation, and are non-trivial enough that
we don't want to copy them into each target's implementation. Instead, we
implement such functions in
[generic_ops-inl.h](../hwy/ops/generic_ops-inl.h), which is included
into every backend. To allow some targets to override these functions, we use
the same per-target include guard mechanism, e.g. `HWY_NATIVE_AES`.

The functions there are typically templated on the vector and/or tag types. This
is necessary because the vector type depends on the target. Although `Vec128` is
available on most targets, `HWY_SCALAR`, `HWY_RVV` and `HWY_SVE*` lack this
type. To enable specialized overloads (e.g. only for signed integers), we use
the `HWY_IF` SFINAE helpers. Example: `template <class V, class D = DFromV<V>,
HWY_IF_SIGNED_D(D)>`. Note that there is a limited set of `HWY_IF` that work
directly with vectors, identified by their `_V` suffix. However, the functions
likely use a `D` type anyway, thus it is convenient to obtain one in the
template arguments and also use that for `HWY_IF_*_D`.

For x86, we also avoid some duplication by implementing only once the functions
which are shared between all targets. They reside in
[x86_128-inl.h](../hwy/ops/x86_128-inl.h) and are also templated on the
vector type.

## Adding a new op

Adding an op consists of three steps, listed below. As an example, consider
https://github.com/google/highway/commit/6c285d64ae50e0f48866072ed3a476fc12df5ab6.

1) Document the new op in `g3doc/quick_reference.md` with its function signature
   and a description of what the op does.

2) Implement the op in each `ops/*-inl.h` header. There are two exceptions,
   detailed in the previous section: first, `generic_ops-inl.h` is not changed in
   the common case where the op has a unique definition for every target. Second,
   if the op's definition would be duplicated in `x86_256-inl.h` and
   `x86_512-inl.h`, it may be expressed as a template in `x86_128-inl.h` with a
   `class V` template argument, e.g. `TableLookupBytesOr0`.

3) Pick the appropriate `hwy/tests/*_test.cc` and add a test. This is also a
   three step process: first define a functor that implements the test logic (e.g.
   `TestPlusMinus`), then a function (e.g. `TestAllPlusMinus`) that invokes this
   functor for all lane types the op supports, and finally a line near the end of
   the file that invokes the function for all targets:
   `HWY_EXPORT_AND_TEST_P(HwyArithmeticTest, TestAllPlusMinus);`. Note the naming
   convention that the function has the same name as the functor except for the
   `TestAll` prefix.

## Reducing the number of overloads via templates

Most ops are supported for many types. Often it is possible to reuse the same
implementation. When this works for every possible type, we simply use a
template. C++ provides several mechanisms for constraining the types:

*   We can extend templates with SFINAE. Highway provides some internal-only
    `HWY_IF_*` macros for this, e.g. `template <typename T, HWY_IF_FLOAT(T)>
    bool IsFiniteT(T t) {`. Variants of these with `_D` and `_V` suffixes exist
    for when the argument is a tag or vector type. Although convenient and
    fairly readable, this style sometimes encounters limits in compiler support,
    especially with older MSVC.

*   When the implementation is lengthy and only a few types are supported, it
    can make sense to move the implementation into namespace detail and provide
    one non-template overload for each type; each calls the implementation.

*   When the implementation only depends on the size in bits of the lane type
    (instead of whether it is signed/float), we sometimes add overloads with an
    additional `SizeTag` argument to namespace detail, and call those from the
    user-visible template. This may avoid compiler limitations relating to the
    otherwise equivalent `HWY_IF_T_SIZE(T, 1)`.

## Deducing the Simd<> argument type

For functions that take a `d` argument such as `Load`, we usually deduce it as a
`class D` template argument rather than deducing the individual `T`, `N`,
`kPow2` arguments to `Simd`. To obtain `T` e.g. for the pointer argument to
`Load`, use `TFromD<D>`. Rather than `N`, e.g. for stack-allocated arrays on
targets where `!HWY_HAVE_SCALABLE`, use `MaxLanes(d)`, or where no `d` lvalue is
available, `HWY_MAX_LANES_D(D)`.

When there are constraints, such as "only enable when the `D` is exactly 128
bits", be careful not to use `Full128<T>` as the function argument type, because
this will not match `Simd<T, 8 / sizeof(T), 1>`, i.e. twice a half-vector.
Instead use `HWY_IF_V_SIZE_D(D, 16)`.

We could perhaps skip the `HWY_IF_V_SIZE_D` if fixed-size vector or mask
arguments are present, because they already have the same effect of overload
resolution. For example, when the arguments are `Vec256` the overload defined in
x86_256 will be selected. However, also verifying the `D` matches the other
arguments helps prevent erroneous or questionable code from compiling. For
example, passing a different `D` to `Store` than the one used to create the
vector argument might point to an error; both should match.

For functions that accept multiple vector types (these are mainly in x86_128,
and avoid duplicating those functions in x86_256 and x86_512), we use
`VFrom<D>`.

## Documentation of platform-specific intrinsics

When adding a new op, it is often necessary to consult the reference for each
platform's intrinsics.

For x86 targets `HWY_SSE2`, `HWY_SSSE3`, `HWY_SSE4`, `HWY_AVX2`, `HWY_AVX3`,
`HWY_AVX3_DL`, `HWY_AVX3_ZEN4`, `HWY_AVX3_SPR` Intel provides a
[searchable reference](https://www.intel.com/content/www/us/en/docs/intrinsics-guide).

For Arm targets `HWY_NEON`, `HWY_NEON_WITHOUT_AES`, `HWY_NEON_BF16`, `HWY_SVE`
(plus its specialization for 256-bit vectors `HWY_SVE_256`), `HWY_SVE2` (plus
its specialization for 128-bit vectors `HWY_SVE2_128`), Arm provides a
[searchable reference](https://developer.arm.com/architectures/instruction-sets/intrinsics).

For RISC-V target `HWY_RVV`, we refer to the assembly language
[specification](https://github.com/riscv/riscv-v-spec/blob/master/v-spec.adoc)
plus the separate
[intrinsics specification](https://github.com/riscv-non-isa/rvv-intrinsic-doc).

For WebAssembly target `HWY_WASM`, we recommend consulting the
[intrinsics header](https://github.com/llvm/llvm-project/blob/main/clang/lib/Headers/wasm_simd128.h).
There is also an unofficial
[searchable list of intrinsics](https://nemequ.github.io/waspr/intrinsics).

For POWER targets `HWY_PPC8`, `HWY_PPC9`, `HWY_PPC10`, there is
[documentation of intrinsics](https://files.openpower.foundation/s/9nRDmJgfjM8MpR7),
the [ISA](https://files.openpower.foundation/s/dAYSdGzTfW4j2r2), plus a
[searchable reference](https://www.ibm.com/docs/en/openxl-c-and-cpp-aix/17.1.1?).

For ZVector targets `HWY_Z14`, `HWY_Z15`, `HWY_Z16`, there is the
[ISA](https://www.ibm.com/support/pages/zarchitecture-principles-operation)
(requires IBMid login), plus a
[searchable reference](https://www.ibm.com/docs/en/zos/2.5.0?topic=topics-using-vector-programming-support).

For LoongArch, there is a
[list of intrinsics](https://jia.je/unofficial-loongarch-intrinsics-guide/lsx/integer_computation/)
and
[ISA reference](https://loongson.github.io/LoongArch-Documentation/LoongArch-Vol1-EN.html).

## Why scalar target

There can be various reasons to avoid using vector intrinsics:

*   The current CPU may not support any instruction sets generated by Highway
    (on x86, we only target S-SSE3 or newer because its predecessor SSE3 was
    introduced in 2004 and it seems unlikely that many users will want to
    support such old CPUs);
*   The compiler may crash or emit incorrect code for certain intrinsics or
    instruction sets;
*   We may want to estimate the speedup from the vector implementation compared
    to scalar code.

Highway provides either the `HWY_SCALAR` or the `HWY_EMU128` target for such
use-cases. Both implement ops using standard C++ instead of intrinsics. They
differ in the vector size: the former always uses single-lane vectors and thus
cannot implement ops such as `AESRound` or `TableLookupBytes`. The latter
guarantees 16-byte vectors are available like all other Highway targets, and
supports all ops. Both of these alternatives are slower than native vector code,
but they allow testing your code even when actual vectors are unavailable.

One of the above targets is used if the CPU does not support any actual SIMD
target. To avoid compiling any intrinsics, define `HWY_COMPILE_ONLY_EMU128`.

`HWY_SCALAR` is only enabled/used `#ifdef HWY_COMPILE_ONLY_SCALAR` (or `#if
HWY_BROKEN_EMU128`). Projects that intend to use it may require `#if HWY_TARGET
!= HWY_SCALAR` around the ops it does not support to prevent compile errors.

---

# API synopsis / quick reference

## High-level overview

Highway is a collection of 'ops': platform-agnostic pure functions that operate
on tuples (multiple values of the same type). These functions are implemented
using platform-specific intrinsics, which map to SIMD/vector instructions.

Your code calls these ops and uses them to implement the desired algorithm.
Alternatively, `hwy/contrib` also includes higher-level algorithms such as
`FindIf` or `VQSort` implemented using these ops.

## Static vs. dynamic dispatch

Highway supports two ways of deciding which instruction sets to use: static or
dynamic dispatch.

Static means targeting a single instruction set, typically the best one enabled
by the given compiler flags. This has no runtime overhead and only compiles your
code once, but because compiler flags are typically conservative, you will not
benefit from more recent instruction sets. Conversely, if you run the binary on
a CPU that does not support this instruction set, it will crash.

Dynamic dispatch means compiling your code multiple times and choosing the best
available implementation at runtime. Highway supports three ways of doing this:

*   Highway can take care of everything including compilation (by re-`#include`
    your code), setting the required compiler #pragmas, and dispatching to the
    best available implementation. The only changes to your code relative to
    static dispatch are adding `#define HWY_TARGET_INCLUDE`, `#include
    "third_party/highway/hwy/foreach_target.h"` (which must come before any
    inclusion of highway.h) and calling `HWY_DYNAMIC_DISPATCH` instead of
    `HWY_STATIC_DISPATCH`.

*   Some build systems (e.g. Apple) support the concept of 'fat' binaries which
    contain code for multiple architectures or instruction sets. Then, the
    operating system or loader typically takes care of calling the appropriate
    code. Highway interoperates with this by using the instruction set requested
    by the current compiler flags during each compilation pass. Your code is the
    same as with static dispatch.

    Note that this method replicates the entire binary, whereas the
    Highway-assisted dynamic dispatch method only replicates your SIMD code,
    which is typically a small fraction of the total size.

*   Because Highway is a library (as opposed to a code generator or compiler),
    the dynamic dispatch method can be inspected, and made to interoperate with
    existing systems. For compilation, you can replace foreach_target.h if your
    build system supports compiling for multiple targets. For choosing the best
    available target, you can replace Highway's CPU detection and decision with
    your own. `HWY_DYNAMIC_DISPATCH` calls into a table of function pointers
    with a zero-based index indicating the desired target. Instead of calling it
    immediately, you can also save the function pointer returned by
    `HWY_DYNAMIC_POINTER`. Note that `HWY_DYNAMIC_POINTER` returns the same
    pointer that `HWY_DYNAMIC_DISPATCH` would. When either of them are first
    invoked, the function pointer first detects the CPU, then calls your actual
    function. You can call `GetChosenTarget().Update(SupportedTargets());` to
    ensure future dynamic dispatch avoids the overhead of CPU detection.
    You can also replace the table lookup with your own choice of index, or even
    call e.g. `N_AVX2::YourFunction` directly.

Examples of both static and dynamic dispatch are provided in examples/.
Typically, the function that does the dispatch receives a pointer to one or more
arrays. Due to differing ABIs, we recommend only passing vector arguments to
functions that are inlined, and in particular not the top-level function that
does the dispatch.

Note that if your compiler is pre-configured to generate code only for a
specific architecture, or your build flags include -m flags that specify a
baseline CPU architecture, then this can interfere with dynamic dispatch, which
aims to build code for all attainable targets. One example is specializing for a
Raspberry Pi CPU that lacks AES, by specifying `-march=armv8-a+crc`. When we
build the `HWY_NEON` target (which would only be used if the CPU actually does
have AES), there is a conflict between the `arch=armv8-a+crypto` that is set via
pragma only for the vector code, and the global `-march`. This results in a
compile error, see #1460, #1570, and #1707. As a workaround, we recommend
avoiding -m flags if possible, and otherwise defining `HWY_COMPILE_ONLY_STATIC`
or `HWY_SKIP_NON_BEST_BASELINE` when building Highway as well as any user code
that includes Highway headers. As a result, only the baseline target, or targets
at least as good as the baseline, will be compiled. Note that it is fine for
user code to still call `HWY_DYNAMIC_DISPATCH`. When Highway is only built for a
single target, `HWY_DYNAMIC_DISPATCH` results in the same direct call that
`HWY_STATIC_DISPATCH` would produce.

## Headers

The public headers are:

*   hwy/highway.h: main header, included from source AND/OR header files that
    use vector types. Note that including in headers may increase compile time,
    but allows declaring functions implemented out of line.

*   hwy/base.h: included from headers that only need compiler/platform-dependent
    definitions (e.g. `PopCount`) without the full highway.h.

*   hwy/foreach_target.h: re-includes the translation unit (specified by
    `HWY_TARGET_INCLUDE`) once per enabled target to generate code from the same
    source code. highway.h must still be included.

*   hwy/aligned_allocator.h: defines functions for allocating memory with
    alignment suitable for `Load`/`Store`.

*   hwy/cache_control.h: defines standalone functions to control caching (e.g.
    prefetching), independent of actual SIMD.

*   hwy/nanobenchmark.h: library for precisely measuring elapsed time (under
    varying inputs) for benchmarking small/medium regions of code.

*   hwy/print-inl.h: defines Print() for writing vector lanes to stderr.

*   hwy/tests/test_util-inl.h: defines macros for invoking tests on all
    available targets, plus per-target functions useful in tests.

Highway provides helper macros to simplify your vector code and ensure support
for dynamic dispatch. To use these, add the following to the start and end of
any vector code:

```
#include "hwy/highway.h"
HWY_BEFORE_NAMESPACE();  // at file scope
namespace project {  // optional
namespace HWY_NAMESPACE {

// implementation

// NOLINTNEXTLINE(google-readability-namespace-comments)
}  // namespace HWY_NAMESPACE
}  // namespace project - optional
HWY_AFTER_NAMESPACE();
```

If you choose not to use the `BEFORE/AFTER` lines, you must prefix any function
that calls Highway ops such as `Load` with `HWY_ATTR`. Either of these will set
the compiler #pragma required to generate vector code.

The `HWY_NAMESPACE` lines ensure each instantiation of your code (one per
target) resides in a unique namespace, thus preventing ODR violations. You can
omit this if your code will only ever use static dispatch.

## Notation in this doc

By vector 'lanes', we mean the 'elements' of that vector. Analogous to the lanes
of a highway or swimming pool, most operations act on each lane independently,
but it is possible for lanes to interact and change order via 'swizzling' ops.

*   `T` denotes the type of a vector lane (integer or floating-point);
*   `N` is a size_t value that governs (but is not necessarily identical to) the
    number of lanes;
*   `D` is shorthand for a zero-sized tag type `Simd<T, N, kPow2>`, used to
    select the desired overloaded function (see next section). Use aliases such
    as `ScalableTag` instead of referring to this type directly;
*   `d` is an lvalue of type `D`, passed as a function argument e.g. to Zero;
*   `V` is the type of a vector, which may be a class or built-in type.
*   `v[i]` is analogous to C++ array notation, with zero-based index `i` from
    the starting address of the vector `v`.

## Vector and tag types

Highway vectors consist of one or more 'lanes' of the same built-in type `T`:
`uint##_t, int##_t` for `## = 8, 16, 32, 64`, or `float##_t` for `## = 16, 32,
64` and `bfloat16_t`. `T` may be retrieved via `TFromD<D>`.
`IsIntegerLaneType<T>` evaluates to true for these `int` or `uint` types.

Beware that `char` may differ from these types, and is not supported directly.
If your code loads from/stores to `char*`, use `T=uint8_t` for Highway's `d`
tags (see below) or `T=int8_t` (which may enable faster less-than/greater-than
comparisons), and cast your `char*` pointers to your `T*`.

In Highway, `float16_t` (an IEEE binary16 half-float) and `bfloat16_t` (the
upper 16 bits of an IEEE binary32 float) only support load, store, and
conversion to/from `float32_t`. The behavior of infinity and NaN in `float16_t`
is implementation-defined due to Armv7. To ensure binary compatibility, these
types are always wrapper structs and cannot be initialized with values directly.
You can initialize them via `BitCastScalar` or `ConvertScalarTo`.

On RVV/SVE, vectors are sizeless and cannot be wrapped inside a class. The
Highway API allows using built-in types as vectors because operations are
expressed as overloaded functions. Instead of constructors, overloaded
initialization functions such as `Set` take a zero-sized tag argument called `d`
of type `D` and return an actual vector of unspecified type.

The actual lane count (used to increment loop counters etc.) can be obtained via
`Lanes(d)`. To improve code generation (constant-propagation) for targets with
fixed-size vectors, this function is `constexpr` `#if HWY_HAVE_CONSTEXPR_LANES`.
Otherwise, users must not assign `Lanes(d)` to `constexpr` variables. You can
ensure this by using `HWY_LANES_CONSTEXPR` instead of `constexpr`. To help
detect mismatches, we define `HWY_HAVE_CONSTEXPR_LANES` to 0 in debug builds.

Because `Lanes(d)` might not be constexpr, it must also not be used as array
dimensions. Instead, storage for vectors should be dynamically allocated, e.g.
via `AllocateAligned(Lanes(d))`.

Note that `Lanes(d)` could potentially change at runtime. This is currently
unlikely, and will not be initiated by Highway without user action, but could
still happen in other circumstances:

*   upon user request in future via special CPU instructions (switching to
    'streaming SVE' mode for Arm SME), or
*   via system software (`prctl(PR_SVE_SET_VL` on Linux for Arm SVE). When the
    vector length is changed using this mechanism, all but the lower 128 bits of
    vector registers are invalidated.

Thus we discourage caching the result; it is typically used inside a function or
basic block. If the application anticipates that one of the above circumstances
could happen, it should ensure by some out-of-band mechanism that such changes
will not happen during the critical section (the vector code which uses the
result of the previously obtained `Lanes(d)`).

`MaxLanes(d)` returns a (potentially loose) upper bound on `Lanes(d)`, and is
always implemented as a constexpr function.

The actual lane count is guaranteed to be a power of two, even on SVE. This
simplifies alignment: remainders can be computed as `count & (Lanes(d) - 1)`
instead of an expensive modulo. It also ensures loop trip counts that are a
large power of two (at least `MaxLanes`) are evenly divisible by the lane count,
thus avoiding the need for a second loop to handle remainders.

`d` lvalues (a tag, NOT actual vector) are obtained using aliases:

*   Most common: `ScalableTag<T[, kPow2=0]> d;` or the macro form `HWY_FULL(T[,
    LMUL=1]) d;`. With the default value of the second argument, these both
    select full vectors which utilize all available lanes.

    Only for targets (e.g. RVV) that support register groups, the kPow2 (-3..3)
    and LMUL argument (1, 2, 4, 8) specify `LMUL`, the number of registers in
    the group. This effectively multiplies the lane count in each operation by
    `LMUL`, or left-shifts by `kPow2` (negative values are understood as
    right-shifting by the absolute value). These arguments will eventually be
    optional hints that may improve performance on 1-2 wide machines (at the
    cost of reducing the effective number of registers), but RVV target does not
    yet support fractional `LMUL`. Thus, mixed-precision code (e.g. demoting
    float to uint8_t) currently requires `LMUL` to be at least the ratio of the
    sizes of the largest and smallest type, and smaller `d` to be obtained via
    `Half<DLarger>`.

    For other targets, `kPow2` must lie within [HWY_MIN_POW2, HWY_MAX_POW2]. The
    `*Tag` aliases clamp to the upper bound but your code should ensure the
    lower bound is not exceeded, typically by specializing compile-time
    recursions for `kPow2` = `HWY_MIN_POW2` (this avoids compile errors when
    `kPow2` is low enough that it is no longer a valid shift count).

*   Less common: `CappedTag<T, kCap> d` or the macro form `HWY_CAPPED(T, kCap)
    d;`. These select vectors or masks where *no more than* the largest power of
    two not exceeding `kCap` lanes have observable effects such as
    loading/storing to memory, or being counted by `CountTrue`. The number of
    lanes may also be less; for the `HWY_SCALAR` target, vectors always have a
    single lane. For example, `CappedTag<T, 3>` will use up to two lanes.

*   For applications that require fixed-size vectors: `FixedTag<T, kCount> d;`
    will select vectors where exactly `kCount` lanes have observable effects.
    These may be implemented using full vectors plus additional runtime cost for
    masking in `Load` etc. `kCount` must be a power of two not exceeding
    `HWY_LANES(T)`, which is one for `HWY_SCALAR`. This tag can be used when the
    `HWY_SCALAR` target is anyway disabled (superseded by a higher baseline) or
    unusable (due to use of ops such as `TableLookupBytes`). As a convenience,
    we also provide `Full128<T>`, `Full64<T>` and `Full32<T>` aliases which are
    equivalent to `FixedTag<T, 16 / sizeof(T)>`, `FixedTag<T, 8 / sizeof(T)>`
    and `FixedTag<T, 4 / sizeof(T)>`.

*   The result of `UpperHalf`/`LowerHalf` has half the lanes. To obtain a
    corresponding `d`, use `Half<decltype(d)>`; the opposite is `Twice<>`.

*   `BlockDFromD<D>` returns a `d` with a lane type of `TFromD<D>` and
    `HWY_MIN(HWY_MAX_LANES_D(D), 16 / sizeof(TFromD<D>))` lanes.

User-specified lane counts or tuples of vectors could cause spills on targets
with fewer or smaller vectors. By contrast, Highway encourages vector-length
agnostic code, which is more performance-portable.

For mixed-precision code (e.g. `uint8_t` lanes promoted to `float`), tags for
the smaller types must be obtained from those of the larger type (e.g. via
`Rebind<uint8_t, ScalableTag<float>>`).

## Using unspecified vector types

Vector types are unspecified and depend on the target. Your code could define
vector variables using `auto`, but it is more readable (due to making the type
visible) to use an alias such as `Vec<D>`, or `decltype(Zero(d))`. Similarly,
the mask type can be obtained via `Mask<D>`. Often your code will first define a
`d` lvalue using `ScalableTag<T>`. You may wish to define an alias for your
vector types such as `using VecT = Vec<decltype(d)>`. Do not use undocumented
types such as `Vec128`; these may work on most targets, but not all (e.g. SVE).

Vectors are sizeless types on RVV/SVE. Therefore, vectors must not be used in
arrays/STL containers (use the lane type `T` instead), class members,
static/thread_local variables, new-expressions (use `AllocateAligned` instead),
and sizeof/pointer arithmetic (increment `T*` by `Lanes(d)` instead).

Initializing constants requires a tag type `D`, or an lvalue `d` of that type.
The `D` can be passed as a template argument or obtained from a vector type `V`
via `DFromV<V>`. `TFromV<V>` is equivalent to `TFromD<DFromV<V>>`.

**Note**: Let `DV = DFromV<V>`. For built-in `V` (currently necessary on
RVV/SVE), `DV` might not be the same as the `D` used to create `V`. In
particular, `DV` must not be passed to `Load/Store` functions because it may
lack the limit on `N` established by the original `D`. However, `Vec<DV>` is the
same as `V`.

Thus a template argument `V` suffices for generic functions that do not load
from/store to memory: `template<class V> V Mul4(V v) { return Mul(v,
Set(DFromV<V>(), 4)); }`.

Example of mixing partial vectors with generic functions:

```
CappedTag<int16_t, 2> d2;
auto v = Mul4(Set(d2, 2));
Store(v, d2, ptr);  // Use d2, NOT DFromV<decltype(v)>()
```

## Targets

Let `Target` denote an instruction set, one of `SCALAR/EMU128`, `RVV`,
`SSE2/SSSE3/SSE4/AVX2/AVX3/AVX3_DL/AVX3_ZEN4/AVX3_SPR` (x86), `PPC8/PPC9/PPC10`
(POWER), `Z14/Z15` (IBM Z), `WASM/WASM_EMU256` (WebAssembly),
`NEON_WITHOUT_AES/NEON/NEON_BF16/SVE/SVE2/SVE_256/SVE2_128` (Arm).

Note that x86 CPUs are segmented into dozens of feature flags and capabilities,
which are often used together because they were introduced in the same CPU
(example: AVX2 and FMA). To keep the number of targets and thus compile time and
code size manageable, we define targets as 'clusters' of related features. To
use `HWY_AVX2`, it is therefore insufficient to pass -mavx2. For definitions of
the clusters, see `kGroup*` in `targets.cc`. The corresponding Clang/GCC
compiler options to enable them (without -m prefix) are defined by
`HWY_TARGET_STR*` in `set_macros-inl.h`, and also listed as comments in
https://gcc.godbolt.org/z/rGnjMevKG.

Targets are only used if enabled (i.e. not broken nor disabled). Baseline
targets are those for which the compiler is unconditionally allowed to generate
instructions (implying the target CPU must support them).

*   `HWY_STATIC_TARGET` is the best enabled baseline `HWY_Target`, and matches
    `HWY_TARGET` in static dispatch mode. This is useful even in dynamic
    dispatch mode for deducing and printing the compiler flags.

*   `HWY_TARGETS` indicates which targets to generate for dynamic dispatch, and
    which headers to include. It is determined by configuration macros and
    always includes `HWY_STATIC_TARGET`.

*   `HWY_SUPPORTED_TARGETS` is the set of targets available at runtime. Expands
    to a literal if only a single target is enabled, or SupportedTargets().

*   `HWY_TARGET`: which `HWY_Target` is currently being compiled. This is
    initially identical to `HWY_STATIC_TARGET` and remains so in static dispatch
    mode. For dynamic dispatch, this changes before each re-inclusion and
    finally reverts to `HWY_STATIC_TARGET`. Can be used in `#if` expressions to
    provide an alternative to functions which are not supported by `HWY_SCALAR`.

    In particular, for x86 we sometimes wish to specialize functions for AVX-512
    because it provides many new instructions. This can be accomplished via `#if
    HWY_TARGET <= HWY_AVX3`, which means AVX-512 or better (e.g. `HWY_AVX3_DL`).
    This is because numerically lower targets are better, and no other platform
    has targets numerically less than those of x86.

*   `HWY_WANT_SSSE3`, `HWY_WANT_SSE4`: add SSSE3 and SSE4 to the baseline even
    if they are not marked as available by the compiler. On MSVC, the only ways
    to enable SSSE3 and SSE4 are defining these, or enabling AVX.

*   `HWY_VISIT_TARGETS(VISITOR)` expands to `VISITOR(HWY_AVX2, N_AVX2)` for all
    enabled targets (here: AVX2). The latter is the namespace name. This can be
    used to declare target-specific functions in a header, so that they can be
    called from within that namespace without the overhead of an additional
    `HWY_DYNAMIC_DISPATCH`. Note that the `foreach_targets.h` mechanism does not
    work for that because it must reside in a .cc file.

You can detect and influence the set of supported targets:

*   `TargetName(t)` returns a string literal identifying the single target `t`,
    where `t` is typically `HWY_TARGET`.

*   `SupportedTargets()` returns an int64_t bitfield of enabled targets that are
    supported on this CPU. The return value may change after calling
    `DisableTargets`, but will never be zero.

*   `HWY_SUPPORTED_TARGETS` is equivalent to `SupportedTargets()` but more
    efficient if only a single target is enabled.

*   `DisableTargets(b)` causes subsequent `SupportedTargets()` to not return
    target(s) whose bits are set in `b`. This is useful for disabling specific
    targets if they are unhelpful or undesirable, e.g. due to memory bandwidth
    limitations. The effect is not cumulative; each call overrides the effect of
    all previous calls. Calling with `b == 0` restores the original behavior.
    Use `SetSupportedTargetsForTest` instead of this function for iteratively
    enabling specific targets for testing.

*   `SetSupportedTargetsForTest(b)` causes subsequent `SupportedTargets` to
    return `b`, minus those disabled via `DisableTargets`. `b` is typically
    derived from a subset of `SupportedTargets()`, e.g. each individual bit in
    order to test each supported target. Calling with `b == 0` restores the
    normal `SupportedTargets` behavior.

## Operations

In the following, the argument or return type `V` denotes a vector with `N`
lanes, and `M` a mask. Operations limited to certain vector types begin with a
constraint of the form `V`: `{prefixes}[{bits}]`. The prefixes `u,i,f` denote
unsigned, signed, and floating-point types, and bits indicates the number of
bits per lane: 8, 16, 32, or 64. Any combination of the specified prefixes and
bits are allowed. Abbreviations of the form `u32 = {u}{32}` may also be used.

Note that Highway functions reside in `hwy::HWY_NAMESPACE`, whereas user-defined
functions reside in `project::[nested]::HWY_NAMESPACE`. Highway functions
generally take either a `D` or vector/mask argument. For targets where vectors
and masks are defined in namespace `hwy`, the functions will be found via
Argument-Dependent Lookup. However, this does not work for function templates,
and RVV and SVE both use built-in vectors. Thus portable code must use one of
the three following options, in descending order of preference:

-   `namespace hn = hwy::HWY_NAMESPACE;` alias used to prefix ops, e.g.
    `hn::LoadDup128(..)`;
-   `using hwy::HWY_NAMESPACE::LoadDup128;` declarations for each op used;
-   `using hwy::HWY_NAMESPACE;` directive. This is generally discouraged,
    especially for SIMD code residing in a header.

Note that overloaded operators were not supported on `RVV` and `SVE` until
recently. Unfortunately, clang's `SVE` comparison operators return integer
vectors instead of the `svbool_t` type which exists for this purpose. To ensure
your code works on all targets, we recommend instead using the corresponding
equivalents mentioned in our description of each overloaded operator, especially
for comparisons, for example `Lt` instead of `operator<`.

<!-- mdlint off(HTML_FORMAT) -->

### Initialization

*   <code>V **Zero**(D)</code>: returns N-lane vector with all bits set to 0.
*   <code>V **Set**(D, T)</code>: returns N-lane vector with all lanes equal to
    the given value of type `T`.
*   <code>V **Undefined**(D)</code>: returns uninitialized N-lane vector, e.g.
    for use as an output parameter.
*   <code>V **Iota**(D, T2)</code>: returns N-lane vector where the lane with
    index `i` has the given value of type `T2` (the op converts it to T) + `i`.
    The least significant lane has index 0. This is useful in tests for
    detecting lane-crossing bugs.
*   <code>V **SignBit**(D)</code>: returns N-lane vector with all lanes set to a
    value whose representation has only the most-significant bit set.
*   <code>V **Dup128VecFromValues**(D d, T t0, .., T tK)</code>: Creates a
    vector from `K+1` values, broadcasted to each 128-bit block if `Lanes(d) >=
    16/sizeof(T)` is true, where `K` is `16/sizeof(T) - 1`.

    Dup128VecFromValues returns the following values in each 128-bit block of
    the result, with `t0` in the least-significant (lowest-indexed) lane of each
    128-bit block and `tK` in the most-significant (highest-indexed) lane of
    each 128-bit block: `{t0, t1, ..., tK}`

*   <code>V **MaskedSetOr**(V no, M m, T a)</code>: returns N-lane vector with
    lane `i` equal to `a` if `m[i]` is true else `no[i]`.

*   <code>V **MaskedSet**(D d, M m, T a)</code>: returns N-lane vector with lane
    `i` equal to `a` if `m[i]` is true else 0.

### Getting/setting lanes

*   <code>T **GetLane**(V)</code>: returns lane 0 within `V`. This is useful for
    extracting `SumOfLanes` results.

The following may be slow on some platforms (e.g. x86) and should not be used in
time-critical code:

*   <code>T **ExtractLane**(V, size_t i)</code>: returns lane `i` within `V`.
    `i` must be in `[0, Lanes(DFromV<V>()))`. Potentially slow, it may be better
    to store an entire vector to an array and then operate on its elements.

*   <code>V **InsertLane**(V, size_t i, T t)</code>: returns a copy of V whose
    lane `i` is set to `t`. `i` must be in `[0, Lanes(DFromV<V>()))`.
    Potentially slow, it may be better set all elements of an aligned array and
    then `Load` it.

### Getting/setting blocks

*   <code>Vec<BlockDFromD<DFromV<V>>> **ExtractBlock**&lt;int kBlock&gt;(V)
    </code>: returns block `kBlock` of V, where `kBlock` is an index to a block
    that is `HWY_MIN(DFromV<V>().MaxBytes(), 16)` bytes.

    `kBlock` must be in `[0, DFromV<V>().MaxBlocks())`.

*   <code>V **InsertBlock**&lt;int kBlock&gt;(V v, Vec<BlockDFromD<DFromV<V>>>
    blk_to_insert)</code>: Inserts `blk_to_insert`, with `blk_to_insert[i]`
    inserted into lane `kBlock * (16 / sizeof(TFromV<V>)) + i` of the result
    vector, if `kBlock * 16 < Lanes(DFromV<V>()) * sizeof(TFromV<V>)` is true.

    Otherwise, returns `v` if `kBlock * 16` is greater than or equal to
    `Lanes(DFromV<V>()) * sizeof(TFromV<V>)`.

    `kBlock` must be in `[0, DFromV<V>().MaxBlocks())`.

*   <code>size_t **Blocks**(D d)</code>: Returns the number of 16-byte blocks
    if `Lanes(d) * sizeof(TFromD<D>)` is greater than or equal to 16.

    Otherwise, returns 1 if `Lanes(d) * sizeof(TFromD<D>)` is less than 16.

### Printing

*   <code>V **Print**(D, const char* caption, V [, size_t lane][, size_t
    max_lanes])</code>: prints `caption` followed by up to `max_lanes`
    comma-separated lanes from the vector argument, starting at index `lane`.
    Defined in hwy/print-inl.h, also available if hwy/tests/test_util-inl.h has
    been included.

### Tuples

As a partial workaround to the "no vectors as class members" compiler limitation
mentioned in "Using unspecified vector types", we provide special types able to
carry 2, 3 or 4 vectors, denoted `Tuple{2-4}` below. Their type is unspecified,
potentially built-in, so use the aliases `Vec{2-4}<D>`. These can (only)
be passed as arguments or returned from functions, and created/accessed using
the functions in this section.

*   <code>Tuple2 **Create2**(D, V v0, V v1)</code>: returns tuple such that
    `Get2<1>(tuple)` returns `v1`.
*   <code>Tuple3 **Create3**(D, V v0, V v1, V v2)</code>: returns tuple such
    that `Get3<2>(tuple)` returns `v2`.
*   <code>Tuple4 **Create4**(D, V v0, V v1, V v2, V v3)</code>: returns tuple
    such that `Get4<3>(tuple)` returns `v3`.

The following take a `size_t` template argument indicating the zero-based index,
from left to right, of the arguments passed to `Create{2-4}`.

*   <code>V **Get2&lt;size_t&gt;**(Tuple2)</code>: returns the i-th vector
    passed to `Create2`.
*   <code>V **Get3&lt;size_t&gt;**(Tuple3)</code>: returns the i-th vector
    passed to `Create3`.
*   <code>V **Get4&lt;size_t&gt;**(Tuple4)</code>: returns the i-th vector
    passed to `Create4`.

*   <code>Tuple2 **Set2&lt;size_t&gt;**(Tuple2 tuple, Vec v)</code>: sets the i-th vector

*   <code>Tuple3 **Set3&lt;size_t&gt;**(Tuple3 tuple, Vec v)</code>: sets the i-th vector

*   <code>Tuple4 **Set4&lt;size_t&gt;**(Tuple4 tuple, Vec v)</code>: sets the i-th vector

### Arithmetic

*   <code>V **operator+**(V a, V b)</code>: returns `a[i] + b[i]` (mod 2^bits).
    Currently unavailable on SVE/RVV; use the equivalent `Add` instead.
*   <code>V **operator-**(V a, V b)</code>: returns `a[i] - b[i]` (mod 2^bits).
    Currently unavailable on SVE/RVV; use the equivalent `Sub` instead.

*   <code>V **AddSub**(V a, V b)</code>: returns `a[i] - b[i]` in the even lanes
    and `a[i] + b[i]` in the odd lanes.

    `AddSub(a, b)` is equivalent to `OddEven(Add(a, b), Sub(a, b))` or `Add(a,
    OddEven(b, Neg(b)))`, but `AddSub(a, b)` is more efficient than
    `OddEven(Add(a, b), Sub(a, b))` or `Add(a, OddEven(b, Neg(b)))` on some
    targets.

*   `V`: `{i,f}` \
    <code>V **Neg**(V a)</code>: returns `-a[i]`.

*   `V`: `i` \
    <code>V **SaturatedNeg**(V a)</code>: returns `a[i] == LimitsMin<T>() ?
    LimitsMax<T>() : -a[i]`.

    `SaturatedNeg(a)` is usually more efficient than `IfThenElse(Eq(a, Set(d,
    LimitsMin<T>())), Set(d, LimitsMax<T>()), Neg(a))`.

*   `V`: `{i,f}` \
    <code>V **Abs**(V a)</code> returns the absolute value of `a[i]`; for
    integers, `LimitsMin()` maps to `LimitsMax() + 1`.

*   `V`: `i` \
    <code>V **SaturatedAbs**(V a)</code> returns `a[i] == LimitsMin<T>() ?
    LimitsMax<T>() : (a[i] < 0 ? (-a[i]) : a[i])`.

    `SaturatedAbs(a)` is usually more efficient than `IfThenElse(Eq(a, Set(d,
    LimitsMin<T>())), Set(d, LimitsMax<T>()), Abs(a))`.

*   <code>V **AbsDiff**(V a, V b)</code>: returns `|a[i] - b[i]|` in each lane.

*   <code>V **PairwiseAdd**(D d, V a, V b)</code>: Add consecutive pairs of
    elements. Return the results of a and b interleaved, such that `r[i] =
    a[i] + a[i+1]` for even lanes and `r[i] = b[i-1] + b[i]` for odd lanes.

*   <code>V **PairwiseSub**(D d, V a, V b)</code>: Subtract consecutive pairs of
    elements. Return the results of a and b interleaved, such that `r[i] =
    a[i+1] - a[i]` for even lanes and `r[i] = b[i] - b[i-1]` for odd lanes.

*   `V`: `{i,u}{8,16,32},f{16,32}`, `VW`: `Vec<RepartitionToWide<DFromV<V>>>` \
    <code>VW **SumsOf2**(V v)</code> returns the sums of 2 consecutive lanes,
    promoting each sum into a lane of `TFromV<VW>`.

*   `V`: `{i,u}{8,16}`, `VW`: `Vec<RepartitionToWideX2<DFromV<V>>>` \
    <code>VW **SumsOf4**(V v)</code> returns the sums of 4 consecutive lanes,
    promoting each sum into a lane of `TFromV<VW>`.

*   `V`: `{i,u}8`, `VW`: `Vec<RepartitionToWideX3<DFromV<V>>>` \
    <code>VW **SumsOf8**(V v)</code> returns the sums of 8 consecutive lanes,
    promoting each sum into a lane of `TFromV<VW>`. This is slower on RVV/WASM.

*   `V`: `{i,u}8`, `VW`: `Vec<RepartitionToWideX3<DFromV<V>>>` \
    <code>VW **SumsOf8AbsDiff**(V a, V b)</code> returns the same result as
    `SumsOf8(AbsDiff(a, b))`, but is more efficient on x86.

*   `V`: `{i,u}8`, `VW`: `Vec<RepartitionToWide<DFromV<V>>>` \
    <code>VW **SumsOfAdjQuadAbsDiff**&lt;int kAOffset, int kBOffset&gt;(V a, V
    b)</code> returns the sums of the absolute differences of 32-bit blocks of
    8-bit integers, widened to `MakeWide<TFromV<V>>`.

    `kAOffset` must be between `0` and `HWY_MIN(1, (HWY_MAX_LANES_D(DFromV<V>) -
    1)/4)`.

    `kBOffset` must be between `0` and `HWY_MIN(3, (HWY_MAX_LANES_D(DFromV<V>) -
    1)/4)`.

    SumsOfAdjQuadAbsDiff computes `|a[a_idx] - b[b_idx]| + |a[a_idx+1] -
    b[b_idx+1]| + |a[a_idx+2] - b[b_idx+2]| + |a[a_idx+3] - b[b_idx+3]|` for
    each lane `i` of the result, where `a_idx` is equal to
    `kAOffset*4+((i/8)*16)+(i&7)` and where `b_idx` is equal to
    `kBOffset*4+((i/8)*16)`.

    If `Lanes(DFromV<V>()) < (8 << kAOffset)` is true, then SumsOfAdjQuadAbsDiff
    returns implementation-defined values in any lanes past the first
    (lowest-indexed) lane of the result vector.

    SumsOfAdjQuadAbsDiff is only available if `HWY_TARGET != HWY_SCALAR`.

*   `V`: `{i,u}8`, `VW`: `Vec<RepartitionToWide<DFromV<V>>>` \
    <code>VW **SumsOfShuffledQuadAbsDiff**&lt;int kIdx3, int kIdx2, int kIdx1,
    int kIdx0&gt;(V a, V b)</code> first shuffles `a` as if by the
    `Per4LaneBlockShuffle<kIdx3, kIdx2, kIdx1, kIdx0>(BitCast(
    RepartitionToWideX2<DFromV<V>>(), a))` operation, and then computes the sum
    of absolute differences of 32-bit blocks of 8-bit integers taken from the
    shuffled `a` vector and the `b` vector.

    `kIdx0`, `kIdx1`, `kIdx2`, and `kIdx3` must be between 0 and 3.

    SumsOfShuffledQuadAbsDiff computes `|a_shuf[a_idx] - b[b_idx]| +
    |a_shuf[a_idx+1] - b[b_idx+1]| + |a_shuf[a_idx+2] - b[b_idx+2]| +
    |a_shuf[a_idx+3] - b[b_idx+3]|` for each lane `i` of the result, where
    `a_shuf` is equal to `BitCast(DFromV<V>(), Per4LaneBlockShuffle<kIdx3,
    kIdx2, kIdx1, kIdx0>(BitCast(RepartitionToWideX2<DFromV<V>>(), a))`, `a_idx`
    is equal to `(i/4)*8+(i&3)`, and `b_idx` is equal to `(i/2)*4`.

    If `Lanes(DFromV<V>()) < 16` is true, SumsOfShuffledQuadAbsDiff returns
    implementation-defined results in any lanes where `(i/4)*8+(i&3)+3 >=
    Lanes(d)`.

    The results of SumsOfAdjQuadAbsDiff are implementation-defined if `kIdx0 >=
    Lanes(DFromV<V>()) / 4`.

    The results of any lanes past the first (lowest-indexed) lane of
    SumsOfAdjQuadAbsDiff are implementation-defined if `kIdx1 >=
    Lanes(DFromV<V>()) / 4`.

    SumsOfShuffledQuadAbsDiff is only available if `HWY_TARGET != HWY_SCALAR`.

*   `V`: `{u,i}{8,16}` \
    <code>V **SaturatedAdd**(V a, V b)</code> returns `a[i] + b[i]` saturated to
    the minimum/maximum representable value.

*   `V`: `{u,i}{8,16}` \
    <code>V **SaturatedSub**(V a, V b)</code> returns `a[i] - b[i]` saturated to
    the minimum/maximum representable value.

*   `V`: `{u,i}` \
    <code>V **AverageRound**(V a, V b)</code> returns `(a[i] + b[i] + 1) >> 1`.

*   <code>V **Clamp**(V a, V lo, V hi)</code>: returns `a[i]` clamped to
    `[lo[i], hi[i]]`.

*   <code>V **operator/**(V a, V b)</code>: returns `a[i] / b[i]` in each lane.
    Currently unavailable on SVE/RVV; use the equivalent `Div` instead.

    For integer vectors, `Div(a, b)` returns an implementation-defined value in
    any lanes where `b[i] == 0`.

    For signed integer vectors, `Div(a, b)` returns an implementation-defined
    value in any lanes where `a[i] == LimitsMin<T>() && b[i] == -1`.

*   `V`: `{u,i}` \
    <code>V **operator%**(V a, V b)</code>: returns `a[i] % b[i]` in each lane.
    Currently unavailable on SVE/RVV; use the equivalent `Mod` instead.

    `Mod(a, b)` returns an implementation-defined value in any lanes where
    `b[i] == 0`.

    For signed integer vectors, `Mod(a, b)` returns an implementation-defined
    value in any lanes where `a[i] == LimitsMin<T>() && b[i] == -1`.

*   `V`: `{f}` \
    <code>V **Sqrt**(V a)</code>: returns `sqrt(a[i])`.

*   `V`: `{f}` \
    <code>V **ApproximateReciprocalSqrt**(V a)</code>: returns an approximation
    of `1.0 / sqrt(a[i])`. `sqrt(a) ~= ApproximateReciprocalSqrt(a) * a`. x86
    and PPC provide 12-bit approximations but the error on Arm is closer to 1%.

*   `V`: `{f}` \
    <code>V **ApproximateReciprocal**(V a)</code>: returns an approximation of
    `1.0 / a[i]`.

*   `V`: `{f}` \
    <code>V **GetExponent**(V v)</code>: returns the exponent of `v[i]` as a
    floating-point value. Essentially calculates `floor(log2(x))`.

*   `V`: `{f}`, `VU`: `Vec<RebindToUnsigned<DFromV<V>>>` \
    <code>VU **GetBiasedExponent**(V v)</code>: returns the biased exponent of
    `v[i]` as an unsigned integer value.

#### Min/Max

**Note**: Min/Max corner cases are target-specific and may change. If either
argument is qNaN, x86 SIMD returns the second argument, Armv7 Neon returns NaN,
Wasm is supposed to return NaN but does not always, but other targets actually
uphold IEEE 754-2019 minimumNumber: returning the other argument if exactly one
is qNaN, and NaN if both are.

*   <code>V **Min**(V a, V b)</code>: returns `min(a[i], b[i])`.

*   <code>V **Max**(V a, V b)</code>: returns `max(a[i], b[i])`.

*   <code>V **MinNumber**(V a, V b)</code>: returns `min(a[i], b[i])` if `a[i]`
    and `b[i]` are both non-NaN.

    If one of `a[i]` or `b[i]` is qNaN and the other value is non-NaN,
    `MinNumber(a, b)` returns the non-NaN value.

    If one of `a[i]` or `b[i]` is sNaN and the other value is non-NaN, it is
    implementation-defined whether `MinNumber(a, b)` returns `a[i]` or `b[i]`.

    Otherwise, if `a[i]` and `b[i]` are both NaN, `MinNumber(a, b)` returns NaN.

*   <code>V **MaxNumber**(V a, V b)</code>: returns `max(a[i], b[i])` if `a[i]`
    and `b[i]` are both non-NaN.

    If one of `a[i]` or `b[i]` is qNaN and the other value is non-NaN,
    `MaxNumber(a, b)` returns the non-NaN value.

    If one of `a[i]` or `b[i]` is sNaN and the other value is non-NaN, it is
    implementation-defined whether `MaxNumber(a, b)` returns `a[i]` or `b[i]`.

    Otherwise, if `a[i]` and `b[i]` are both NaN, `MaxNumber(a, b)` returns NaN.

*   <code>V **MinMagnitude**(V a, V b)</code>: returns the number with the
    smaller magnitude if `a[i]` and `b[i]` are both non-NaN values.

    If `a[i]` and `b[i]` are both non-NaN, `MinMagnitude(a, b)` returns
    `(|a[i]| < |b[i]| || (|a[i]| == |b[i]| && a[i] < b[i])) ? a[i] : b[i]`.

    Otherwise, the results of `MinMagnitude(a, b)` are implementation-defined
    if `a[i]` is NaN or `b[i]` is NaN.

*   <code>V **MaxMagnitude**(V a, V b)</code>: returns the number with the
    larger magnitude if `a[i]` and `b[i]` are both non-NaN values.

    If `a[i]` and `b[i]` are both non-NaN, `MaxMagnitude(a, b)` returns
    `(|a[i]| < |b[i]| || (|a[i]| == |b[i]| && a[i] < b[i])) ? b[i] : a[i]`.

    Otherwise, the results of `MaxMagnitude(a, b)` are implementation-defined
    if `a[i]` is NaN or `b[i]` is NaN.

All other ops in this section are only available if `HWY_TARGET != HWY_SCALAR`:

*   `V`: `u64` \
    <code>V **Min128**(D, V a, V b)</code>: returns the minimum of unsigned
    128-bit values, each stored as an adjacent pair of 64-bit lanes (e.g.
    indices 1 and 0, where 0 is the least-significant 64-bits).

*   `V`: `u64` \
    <code>V **Max128**(D, V a, V b)</code>: returns the maximum of unsigned
    128-bit values, each stored as an adjacent pair of 64-bit lanes (e.g.
    indices 1 and 0, where 0 is the least-significant 64-bits).

*   `V`: `u64` \
    <code>V **Min128Upper**(D, V a, V b)</code>: for each 128-bit key-value
    pair, returns `a` if it is considered less than `b` by Lt128Upper, else `b`.

*   `V`: `u64` \
    <code>V **Max128Upper**(D, V a, V b)</code>: for each 128-bit key-value
    pair, returns `a` if it is considered > `b` by Lt128Upper, else `b`.

#### Multiply

*   <code>V <b>operator*</b>(V a, V b)</code>: returns `r[i] = a[i] * b[i]`,
    truncating it to the lower half for integer inputs. Currently unavailable on
    SVE/RVV; use the equivalent `Mul` instead.

*   `V`: `f` <code>V **MulRound**(V a, V b)</code>: Multiplies `a[i]` by `b[i]`
    and rounds the result to the nearest int with ties going to even.

*   `V`: `f`, `VI`: `Vec<RebindToSigned<DFromV<V>>>` \
    <code>V **MulByPow2**(V a, VI b)</code>: Multiplies `a[i]` by `2^b[i]`.

    `MulByPow2(a, b)` is equivalent to `std::ldexp(a[i], HWY_MIN(HWY_MAX(b[i],
    LimitsMin<int>()), LimitsMax<int>()))`.

*   `V`: `f` <code>V **MulByFloorPow2**(V a, V b)</code>: Multiplies `a[i]` by
    `2^floor(b[i])`.

    It is implementation-defined if `MulByFloorPow2(a, b)` returns zero or NaN
    in any lanes where `a[i]` is NaN and `b[i]` is equal to negative infinity.

    It is implementation-defined if `MulByFloorPow2(a, b)` returns positive
    infinity or NaN in any lanes where `a[i]` is NaN and `b[i]` is equal to
    positive infinity.

    If `a[i]` is a non-NaN value and `b[i]` is equal to negative infinity,
    `MulByFloorPow2(a, b)` is equivalent to `a[i] * 0.0`.

    If `b[i]` is NaN or if `a[i]` is non-NaN and `b[i]` is positive infinity,
    `MulByFloorPow2(a, b)` is equivalent to `a[i] * b[i]`.

    If `b[i]` is a finite value, `MulByFloorPow2(a, b)` is equivalent to
    `MulByPow2(a, FloorInt(b))`.

*   `V`: `{u,i}` \
    <code>V **MulHigh**(V a, V b)</code>: returns the upper half of `a[i] *
    b[i]` in each lane.

*   `V`: `i16` \
    <code>V **MulFixedPoint15**(V a, V b)</code>: returns the result of
    multiplying two Q1.15 fixed-point numbers. This corresponds to doubling the
    multiplication result and storing the upper half. Results are
    implementation-defined iff both inputs are -32768.

*   `V`: `{u,i}` \
    <code>V2 **MulEven**(V a, V b)</code>: returns double-wide result of `a[i] *
    b[i]` for every even `i`, in lanes `i` (lower) and `i + 1` (upper). `V2` is
    a vector with double-width lanes, or the same as `V` for 64-bit inputs
    (which are only supported if `HWY_TARGET != HWY_SCALAR`).

*   `V`: `{u,i}` \
    <code>V **MulOdd**(V a, V b)</code>: returns double-wide result of `a[i] *
    b[i]` for every odd `i`, in lanes `i - 1` (lower) and `i` (upper). Only
    supported if `HWY_TARGET != HWY_SCALAR`.

*   `V`: `{bf,u,i}16`, `D`: `RepartitionToWide<DFromV<V>>` \
    <code>Vec&lt;D&gt; **WidenMulPairwiseAdd**(D d, V a, V b)</code>: widens `a`
    and `b` to `TFromD<D>` and computes `a[2*i+1]*b[2*i+1] + a[2*i+0]*b[2*i+0]`.

*   `VI`: `i8`, `VU`: `Vec<RebindToUnsigned<DFromV<VI>>>`, `DI`:
    `RepartitionToWide<DFromV<VI>>` \
    <code>Vec&lt;DI&gt; **SatWidenMulPairwiseAdd**(DI di, VU a_u, VI b_i)
    </code>: widens `a_u` and `b_i` to `TFromD<DI>` and computes
    `a_u[2*i+1]*b_i[2*i+1] + a_u[2*i+0]*b_i[2*i+0]`, saturated to the range of
    `TFromD<D>`.

*   `DW`: `i32`, `D`: `Rebind<MakeNarrow<TFromD<DW>>, DW>`, `VW`: `Vec<DW>`,
    `V`: `Vec<D>` \
    <code>Vec&lt;D&gt; **SatWidenMulPairwiseAccumulate**(DW, V a, V b, VW sum)
    </code>: widens `a[i]` and `b[i]` to `TFromD<DI>` and computes
    `a[2*i]*b[2*i] + a[2*i+1]*b[2*i+1] + sum[i]`, saturated to the range of
    `TFromD<DW>`.

*   `DW`: `i32`, `D`: `Rebind<MakeNarrow<TFromD<DW>>, DW>`, `VW`: `Vec<DW>`,
    `V`: `Vec<D>` \
    <code>VW **SatWidenMulAccumFixedPoint**(DW, V a, V b, VW sum)**</code>:
    First, widens `a` and `b` to `TFromD<DW>`, then adds `a[i] * b[i] * 2` to
    `sum[i]`, saturated to the range of `TFromD<DW>`.

    If `a[i] == LimitsMin<TFromD<D>>() && b[i] == LimitsMin<TFromD<D>>()`, it is
    implementation-defined whether `a[i] * b[i] * 2` is first saturated to
    `TFromD<DW>` prior to the addition of `a[i] * b[i] * 2` to `sum[i]`.

*   `V`: `{bf,u,i}16`, `DW`: `RepartitionToWide<DFromV<V>>`, `VW`: `Vec<DW>` \
    <code>VW **ReorderWidenMulAccumulate**(DW d, V a, V b, VW sum0, VW&
    sum1)</code>: widens `a` and `b` to `TFromD<DW>`, then adds `a[i] * b[i]` to
    either `sum1[j]` or lane `j` of the return value, where `j = P(i)` and `P`
    is a permutation. The only guarantee is that `SumOfLanes(d,
    Add(return_value, sum1))` is the sum of all `a[i] * b[i]`. This is useful
    for computing dot products and the L2 norm. The initial value of `sum1`
    before any call to `ReorderWidenMulAccumulate` must be zero (because it is
    unused on some platforms). It is safe to set the initial value of `sum0` to
    any vector `v`; this has the effect of increasing the total sum by
    `GetLane(SumOfLanes(d, v))` and may be slightly more efficient than later
    adding `v` to `sum0`.

*   `VW`: `{f,u,i}32` \
    <code>VW **RearrangeToOddPlusEven**(VW sum0, VW sum1)</code>: returns in
    each 32-bit lane with index `i` `a[2*i+1]*b[2*i+1] + a[2*i+0]*b[2*i+0]`.
    `sum0` must be the return value of a prior `ReorderWidenMulAccumulate`, and
    `sum1` must be its last (output) argument. In other words, this strengthens
    the invariant of `ReorderWidenMulAccumulate` such that each 32-bit lane is
    the sum of the widened products whose 16-bit inputs came from the top and
    bottom halves of the 32-bit lane. This is typically called after a series of
    calls to `ReorderWidenMulAccumulate`, as opposed to after each one.
    Exception: if `HWY_TARGET == HWY_SCALAR`, returns `a[0]*b[0]`. Note that the
    initial value of `sum1` must be zero, see `ReorderWidenMulAccumulate`.

*   `VN`: `{u,i}{8,16}`, `D`: `RepartitionToWideX2<DFromV<VN>>` \
    <code>Vec&lt;D&gt; **SumOfMulQuadAccumulate**(D d, VN a, VN b, Vec&lt;D&gt;
    sum)</code>: widens `a` and `b` to `TFromD<D>` and computes `sum[i] +
    a[4*i+3]*b[4*i+3] + a[4*i+2]*b[4*i+2] + a[4*i+1]*b[4*i+1] +
    a[4*i+0]*b[4*i+0]`

*   `VN_I`: `i8`, `VN_U`: `Vec<RebindToUnsigned<DFromV<VN_I>>>`, `DI`:
    `Repartition<int32_t, DFromV<VN_I>>` \
    <code>Vec&lt;DI&gt; **SumOfMulQuadAccumulate**(DI di, VN_U a_u, VN_I b_i,
    Vec&lt;DI&gt; sum)</code>: widens `a` and `b` to `TFromD<DI>` and computes
    `sum[i] + a[4*i+3]*b[4*i+3] + a[4*i+2]*b[4*i+2] + a[4*i+1]*b[4*i+1] +
    a[4*i+0]*b[4*i+0]`

*   `V`: `{u,i}{8,16,32},{f}16`, \
    `VW`: `Vec<RepartitionToWide<DFromV<V>>`: \
    `VW **WidenMulAccumulate**(D, V a, V b, VW low, VW& high)`: widens `a` and
    `b`, multiplies them together, then adds them to `Combine(Twice<D>(), high,
    low)`. Returns the lower half of the result, and sets high to the upper
    half.

#### Fused multiply-add

When implemented using special instructions, these functions are more precise
and faster than separate multiplication followed by addition. The `*Sub`
variants are somewhat slower on Arm, and unavailable for integer inputs; if the
`c` argument is a constant, it would be better to negate it and use `MulAdd`.

*   <code>V **MulAdd**(V a, V b, V c)</code>: returns `a[i] * b[i] + c[i]`.

*   <code>V **NegMulAdd**(V a, V b, V c)</code>: returns `-a[i] * b[i] + c[i]`.

*   <code>V **MulSub**(V a, V b, V c)</code>: returns `a[i] * b[i] - c[i]`.

*   <code>V **NegMulSub**(V a, V b, V c)</code>: returns `-a[i] * b[i] - c[i]`.

*   <code>V **MulAddSub**(V a, V b, V c)</code>: returns `a[i] * b[i] - c[i]` in
    the even lanes and `a[i] * b[i] + c[i]` in the odd lanes.

    `MulAddSub(a, b, c)` is equivalent to `OddEven(MulAdd(a, b, c), MulSub(a, b,
    c))` or `MulAddSub(a, b, OddEven(c, Neg(c))`, but `MulSub(a, b, c)` is more
    efficient on some targets (including AVX2/AVX3).

*   <code>V **MulSubAdd**(V a, V b, V c)</code>: returns `a[i] * b[i] + c[i]` in
    the even lanes and `a[i] * b[i] - c[i]` in the odd lanes. Essentially,
    MulAddSub with `c[i]` negated.

*   `V`: `bf16`, `D`: `RepartitionToWide<DFromV<V>>`, `VW`: `Vec<D>` \
    <code>VW **MulEvenAdd**(D d, V a, V b, VW c)</code>: equivalent to and
    potentially more efficient than `MulAdd(PromoteEvenTo(d, a),
    PromoteEvenTo(d, b), c)`.

*   `V`: `bf16`, `D`: `RepartitionToWide<DFromV<V>>`, `VW`: `Vec<D>` \
    <code>VW **MulOddAdd**(D d, V a, V b, VW c)</code>: equivalent to and
    potentially more efficient than `MulAdd(PromoteOddTo(d, a), PromoteOddTo(d,
    b), c)`.

#### Masked arithmetic

All ops in this section return `no` for `mask=false` lanes, and suppress any
exceptions for those lanes if that is supported by the ISA. When exceptions are
not a concern, these are equivalent to, and potentially more efficient than,
`IfThenElse(m, Add(a, b), no);` etc.

*   `V`: `{f}` \
    <code>V **MaskedSqrtOr**(V no, M m, V a)</code>: returns `sqrt(a[i])` or
    `no[i]` if `m[i]` is false.
*   <code>V **MaskedMinOr**(V no, M m, V a, V b)</code>: returns `Min(a, b)[i]`
    or `no[i]` if `m[i]` is false.
*   <code>V **MaskedMaxOr**(V no, M m, V a, V b)</code>: returns `Max(a, b)[i]`
    or `no[i]` if `m[i]` is false.
*   <code>V **MaskedAddOr**(V no, M m, V a, V b)</code>: returns `a[i] + b[i]`
    or `no[i]` if `m[i]` is false.
*   <code>V **MaskedSubOr**(V no, M m, V a, V b)</code>: returns `a[i] - b[i]`
    or `no[i]` if `m[i]` is false.
*   <code>V **MaskedMulOr**(V no, M m, V a, V b)</code>: returns `a[i] * b[i]`
    or `no[i]` if `m[i]` is false.
*   <code>V **MaskedDivOr**(V no, M m, V a, V b)</code>: returns `a[i] / b[i]`
    or `no[i]` if `m[i]` is false.
*   `V`: `{u,i}` \
    <code>V **MaskedModOr**(V no, M m, V a, V b)</code>: returns `a[i] % b[i]`
    or `no[i]` if `m[i]` is false.
*   `V`: `{u,i}{8,16}` \
    <code>V **MaskedSatAddOr**(V no, M m, V a, V b)</code>: returns `a[i] +
    b[i]` saturated to the minimum/maximum representable value, or `no[i]` if
    `m[i]` is false.
*   `V`: `{u,i}{8,16}` \
    <code>V **MaskedSatSubOr**(V no, M m, V a, V b)</code>: returns `a[i] +
    b[i]` saturated to the minimum/maximum representable value, or `no[i]` if
    `m[i]` is false.
*   <code>V **MaskedMulAddOr**(V no, M m, V mul, V x, V add)</code>: returns
    `mul[i] * x[i] + add[i]` or `no[i]` if `m[i]` is false.

*   `V`: `{i,f}` \
    <code>V **MaskedAbsOr**(V no, M m, V a)</code>: returns the absolute value
    of `a[i]` where m is active and returns `no[i]` otherwise.

#### Zero masked arithmetic

All ops in this section return `0` for `mask=false` lanes. These are equivalent
to, and potentially more efficient than, `IfThenElseZero(m, Add(a, b));` etc.

*   `V`: `{i,f}` \
    <code>V **MaskedAbs**(M m, V a)</code>: returns the absolute value of
    `a[i]` where m is active and returns zero otherwise.

*   <code>V **MaskedMax**(M m, V a, V b)</code>: returns `Max(a, b)[i]` or
    `zero` if `m[i]` is false.
*   <code>V **MaskedAdd**(M m, V a, V b)</code>: returns `a[i] + b[i]` or `0` if
    `m[i]` is false.
*   <code>V **MaskedSub**(M m, V a, V b)</code>: returns `a[i] - b[i]` or `0` if
    `m[i]` is false.
*   <code>V **MaskedMul**(M m, V a, V b)</code>: returns `a[i] * b[i]` or `0` if
    `m[i]` is false.
*   <code>V **MaskedDiv**(M m, V a, V b)</code>: returns `a[i] / b[i]` or `0` if
    `m[i]` is false.
*   `V`: `{u,i}{8,16}` \
    <code>V **MaskedSaturatedAdd**(M m, V a, V b)</code>: returns `a[i] + b[i]`
    saturated to the minimum/maximum representable value, or `0` if `m[i]` is
    false.
*   `V`: `{u,i}{8,16}` \
    <code>V **MaskedSaturatedSub**(M m, V a, V b)</code>: returns `a[i] - b[i]`
    saturated to the minimum/maximum representable value, or `0` if `m[i]` is
    false.
*   `V`: `i16` \
    <code>V **MaskedMulFixedPoint15**(M m, V a, V b)</code>: returns returns the
    result of multiplying two Q1.15 fixed-point numbers, or `0` if `m[i]` is
    false.
*   <code>V **MaskedMulAdd**(M m, V a, V b, V c)</code>: returns `a[i] * b[i] +
    c[i]` or `0` if `m[i]` is false.
*   <code>V **MaskedNegMulAdd**(M m, V a, V b, V c)</code>: returns `-a[i] *
    b[i] + c[i]` or `0` if `m[i]` is false.
*   `V`: `{bf,u,i}16`, `D`: `RepartitionToWide<DFromV<V>>` \
    <code>Vec&lt;D&gt; **MaskedWidenMulPairwiseAdd**(D d, M m, V a, V b)</code>:
    widens `a` and `b` to `TFromD<D>` and computes `a[2*i+1]*b[2*i+1] +
    a[2*i+0]*b[2*i+0]`, or `0` if `m[i]` is false.
*   `V`: `{f}` \
    <code>V **MaskedSqrt**(M m, V a)</code>: returns `sqrt(a[i])` where m is
    true, and zero otherwise.
*   `V`: `{f}` \
    <code>V **MaskedApproximateReciprocalSqrt**(M m, V a)</code>: returns the
    result of ApproximateReciprocalSqrt where m is true and zero otherwise.
*   `V`: `{f}` \
    <code>V **MaskedApproximateReciprocal**(M m, V a)</code>: returns the result
    of ApproximateReciprocal where m is true and zero otherwise.

#### Complex number operations

Complex types are represented as complex value pairs of real and imaginary
components, with the real components in even-indexed lanes and the imaginary
components in odd-indexed lanes.

All multiplies in this section are performing complex multiplication,
i.e. `(a + ib)(c + id)`.

Take `j` to be the even values of `i`.

*   `V`: `{f}` \
    <code>V **ComplexConj**(V v)</code>: returns the complex conjugate of the
    vector, this negates the imaginary lanes. This is equivalent to
    `OddEven(Neg(a), a)`.
*   `V`: `{f}` \
    <code>V **MulComplex**(V a, V b)</code>: returns `(a[j] + i.a[j + 1])(b[j] +
    i.b[j + 1])`
*   `V`: `{f}` \
    <code>V **MulComplexConj**(V a, V b)</code>: returns `(a[j] + i.a[j +
    1])(b[j] - i.b[j + 1])`
*   `V`: `{f}` \
    <code>V **MulComplexAdd**(V a, V b, V c)</code>: returns `(a[j] + i.a[j +
    1])(b[j] + i.b[j + 1]) + (c[j] + i.c[j + 1])`
*   `V`: `{f}` \
    <code>V **MulComplexConjAdd**(V a, V b, V c)</code>: returns `(a[j] +
    i.a[j + 1])(b[j] - i.b[j + 1]) + (c[j] + i.c[j + 1])`
*   `V`: `{f}` \
    <code>V **MaskedMulComplexConjAdd**(M mask, V a, V b, V c)</code>: returns
    `(a[j] + i.a[j + 1])(b[j] - i.b[j + 1]) + (c[j] + i.c[j + 1])` or `0` if
    `mask[i]` is false.
*   `V`: `{f}` \
    <code>V **MaskedMulComplexConj**(M mask, V a, V b)</code>: returns `(a[j] +
    i.a[j + 1])(b[j] - i.b[j + 1])` or `0` if `mask[i]` is false.
*   `V`: `{f}` \
    <code>V **MaskedMulComplexOr**(V no, M mask, V a, V b)</code>: returns
    `(a[j] + i.a[j + 1])(b[j] + i.b[j + 1])` or `no[i]` if `mask[i]` is false.

#### Shifts

**Note**: Counts not in `[0, sizeof(T)*8)` yield implementation-defined results.
Left-shifting signed `T` and right-shifting positive signed `T` is the same as
shifting `MakeUnsigned<T>` and casting to `T`. Right-shifting negative signed
`T` is the same as an unsigned shift, except that 1-bits are shifted in.

Compile-time constant shifts: the amount must be in [0, sizeof(T)*8). Generally
the most efficient variant, but 8-bit shifts are potentially slower than other
lane sizes, and `RotateRight` is often emulated with shifts:

*   `V`: `{u,i}` \
    <code>V **ShiftLeft**&lt;int&gt;(V a)</code> returns `a[i] << int`.

*   `V`: `{u,i}` \
    <code>V **ShiftRight**&lt;int&gt;(V a)</code> returns `a[i] >> int`.

*   `V`: `{u,i}` \
    <code>V **RoundingShiftRight**&lt;int&gt;(V a)</code> returns
    `((int == 0) ? a[i] : (((a[i] >> (int - 1)) + 1) >> 1)`.

*   `V`: `{u,i}` \
    <code>V **RotateLeft**&lt;int&gt;(V a)</code> returns `(a[i] << int) |
    (static_cast<TU>(a[i]) >> (sizeof(T)*8 - int))`.

*   `V`: `{u,i}` \
    <code>V **RotateRight**&lt;int&gt;(V a)</code> returns
    `(static_cast<TU>(a[i]) >> int) | (a[i] << (sizeof(T)*8 - int))`.

Shift all lanes by the same (not necessarily compile-time constant) amount:

*   `V`: `{u,i}` \
    <code>V **ShiftLeftSame**(V a, int bits)</code> returns `a[i] << bits`.

*   `V`: `{u,i}` \
    <code>V **ShiftRightSame**(V a, int bits)</code> returns `a[i] >> bits`.

*   `V`: `{u,i}` \
    <code>V **RoundingShiftRightSame**&lt;int kShiftAmt&gt;(V a, int bits)
    </code> returns `((bits == 0) ? a[i] : (((a[i] >> (bits - 1)) + 1) >> 1)`.

*   `V`: `{u,i}` \
    <code>V **RotateLeftSame**(V a, int bits)</code> returns
    `(a[i] << shl_bits) | (static_cast<TU>(a[i]) >>
    (sizeof(T)*8 - shl_bits))`, where `shl_bits` is equal to
    `bits & (sizeof(T)*8 - 1)`.

*   `V`: `{u,i}` \
    <code>V **RotateRightSame**(V a, int bits)</code> returns
    `(static_cast<TU>(a[i]) >> shr_bits) | (a[i] >>
    (sizeof(T)*8 - shr_bits))`, where `shr_bits` is equal to
    `bits & (sizeof(T)*8 - 1)`.

Per-lane variable shifts (slow if SSSE3/SSE4, or 16-bit, or Shr i64 on AVX2):

*   `V`: `{u,i}` \
    <code>V **operator<<**(V a, V b)</code> returns `a[i] << b[i]`. Currently
    unavailable on SVE/RVV; use the equivalent `Shl` instead.

*   `V`: `{u,i}` \
    <code>V **operator>>**(V a, V b)</code> returns `a[i] >> b[i]`. Currently
    unavailable on SVE/RVV; use the equivalent `Shr` instead.

*   `V`: `{u,i}` \
    <code>V **RoundingShr**(V a, V b)</code> returns
    `((b[i] == 0) ? a[i] : (((a[i] >> (b[i] - 1)) + 1) >> 1)`.

*   `V`: `{u,i}` \
    <code>V **Rol**(V a, V b)</code> returns
    `(a[i] << (b[i] & shift_amt_mask)) |
    (static_cast<TU>(a[i]) >> ((sizeof(T)*8 - b[i]) & shift_amt_mask))`,
    where `shift_amt_mask` is equal to `sizeof(T)*8 - 1`.

*   `V`: `{u,i}` \
    <code>V **Ror**(V a, V b)</code> returns
    `(static_cast<TU>(a[i]) >> (b[i] & shift_amt_mask)) |
    (a[i] << ((sizeof(T)*8 - b[i]) & shift_amt_mask))`, where `shift_amt_mask` is
    equal to `sizeof(T)*8 - 1`.

A compound shift on 64-bit values:

*   `V`: `{u,i}64`, `VI`: `{u,i}8` \
    <code>V **MultiRotateRight**(V vals, VI indices)</code>: returns a vector
    with `(vals[i] >> indices[i*8+j]) & 0xff` in byte `j` of vector `r[i]` for
    each `j` between 0 and 7.

    If `indices[i*8+j]` is less than 0 or greater than 63, byte `j` of `r[i]` is
    implementation-defined.

    `VI` must be either `Vec<Repartition<int8_t, DFromV<V>>>` or
    `Vec<Repartition<uint8_t, DFromV<V>>>`.

    `MultiRotateRight(V vals, VI indices)` is equivalent to the following loop
    (where `N` is equal to `Lanes(DFromV<V>())`):

    ```
    for(size_t i = 0; i < N; i++) {
      uint64_t shift_result = 0;
      for(int j = 0; j < 8; j++) {
        uint64_t rot_result =
          (static_cast<uint64_t>(v[i]) >> indices[i*8+j]) |
          (static_cast<uint64_t>(v[i]) << ((-indices[i*8+j]) & 63));
    #if HWY_IS_LITTLE_ENDIAN
        shift_result |= (rot_result & 0xff) << (j * 8);
    #else
        shift_result |= (rot_result & 0xff) << ((j ^ 7) * 8);
    #endif
      }
      r[i] = shift_result;
    }
    ```

#### Masked Shifts

*   `V`: `{u,i}` \
    <code>V **MaskedShiftLeft**&lt;int&gt;(M mask, V a)</code> returns `a[i] <<
    int` or `0` if `mask[i]` is false.

*   `V`: `{u,i}` \
    <code>V **MaskedShiftRight**&lt;int&gt;(M mask, V a)</code> returns `a[i] >>
    int` or `0` if `mask[i]` is false.

*   `V`: `{u,i}` \
    <code>V **MaskedShiftRightOr**&lt;int&gt;(V no, M mask, V a)</code> returns
    `a[i] >> int` or `no[i]` if `mask[i]` is false.

*   `V`: `{u,i}` \
    <code>V **MaskedShrOr**(V no, M mask, V a, V shifts)</code> returns `a[i] >>
    shifts[i]` or `no[i]` if `mask[i]` is false.

#### Floating-point rounding

*   `V`: `{f}` \
    <code>V **Round**(V v)</code>: returns `v[i]` rounded towards the nearest
    integer, with ties to even.

*   `V`: `{f}` \
    <code>V **Trunc**(V v)</code>: returns `v[i]` rounded towards zero
    (truncate).

*   `V`: `{f}` \
    <code>V **Ceil**(V v)</code>: returns `v[i]` rounded towards positive
    infinity (ceiling).

*   `V`: `{f}` \
    <code>V **Floor**(V v)</code>: returns `v[i]` rounded towards negative
    infinity.

#### Floating-point classification

*   `V`: `{f}` \
    <code>M **IsNaN**(V v)</code>: returns mask indicating whether `v[i]` is
    "not a number" (unordered).

*   `V`: `{f}` \
    <code>M **IsEitherNaN**(V a, V b)</code>: equivalent to
    `Or(IsNaN(a), IsNaN(b))`, but `IsEitherNaN(a, b)` is more efficient than
    `Or(IsNaN(a), IsNaN(b))` on x86.

*   `V`: `{f}` \
    <code>M **IsInf**(V v)</code>: returns mask indicating whether `v[i]` is
    positive or negative infinity.

*   `V`: `{f}` \
    <code>M **IsFinite**(V v)</code>: returns mask indicating whether `v[i]` is
    neither NaN nor infinity, i.e. normal, subnormal or zero. Equivalent to
    `Not(Or(IsNaN(v), IsInf(v)))`.

#### Masked floating-point classification

All ops in this section return `false` for `mask=false` lanes. These are
equivalent to, and potentially more efficient than, `And(m, IsNaN(v));` etc.

*   `V`: `{f}` \
    <code>M **MaskedIsNaN**(M m, V v)</code>: returns mask indicating whether
    `v[i]` is "not a number" (unordered) or `false` if `m[i]` is false.

### Logical

*   `V`: `{u,i}` \
    <code>V **PopulationCount**(V a)</code>: returns the number of 1-bits in
    each lane, i.e. `PopCount(a[i])`.

*   `V`: `{u,i}` \
    <code>V **LeadingZeroCount**(V a)</code>: returns the number of
    leading zeros in each lane. For any lanes where ```a[i]``` is zero,
    ```sizeof(TFromV<V>) * 8``` is returned in the corresponding result lanes.

*   `V`: `{u,i}` \
    <code>V **MaskedLeadingZeroCount**(M m, V a)</code>: returns the
    result of LeadingZeroCount where `m[i]` is true, and zero otherwise.

*   `V`: `{u,i}` \
    <code>V **TrailingZeroCount**(V a)</code>: returns the number of
    trailing zeros in each lane. For any lanes where ```a[i]``` is zero,
    ```sizeof(TFromV<V>) * 8``` is returned in the corresponding result lanes.

*   `V`: `{u,i}` \
    <code>V **HighestSetBitIndex**(V a)</code>: returns the index of
    the highest set bit of each lane. For any lanes of a signed vector type
    where ```a[i]``` is zero, an unspecified negative value is returned in the
    corresponding result lanes. For any lanes of an unsigned vector type
    where ```a[i]``` is zero, an unspecified value that is greater than
    ```HighestValue<MakeSigned<TFromV<V>>>()``` is returned in the
    corresponding result lanes.

*   <code>bool **AllBits1**(D, V v)</code>: returns whether all bits are set.

*   <code>bool **AllBits0**(D, V v)</code>: returns whether all bits are clear.

The following operate on individual bits within each lane. Note that the
non-operator functions (`And` instead of `&`) must be used for floating-point
types, and on SVE/RVV.

*   `V`: `{u,i}` \
    <code>V **operator&**(V a, V b)</code>: returns `a[i] & b[i]`. Currently
    unavailable on SVE/RVV; use the equivalent `And` instead.

*   `V`: `{u,i}` \
    <code>V **operator|**(V a, V b)</code>: returns `a[i] | b[i]`. Currently
    unavailable on SVE/RVV; use the equivalent `Or` instead.

*   `V`: `{u,i}` \
    <code>V **operator^**(V a, V b)</code>: returns `a[i] ^ b[i]`. Currently
    unavailable on SVE/RVV; use the equivalent `Xor` instead.

*   `V`: `{u,i}` \
    <code>V **Not**(V v)</code>: returns `~v[i]`.

*   <code>V **AndNot**(V a, V b)</code>: returns `~a[i] & b[i]`.

*   <code>V **MaskedOr**(M m, V a, V b)</code>: returns `a[i] | b[i]`
    or `zero` if `m[i]` is false.

The following three-argument functions may be more efficient than assembling
them from 2-argument functions:

*   <code>V **Xor3**(V x1, V x2, V x3)</code>: returns `x1[i] ^ x2[i] ^ x3[i]`.
    This is more efficient than `Or3` on some targets. When inputs are disjoint
    (no bit is set in more than one argument), `Xor3` and `Or3` are equivalent
    and you should use the former.
*   <code>V **Or3**(V o1, V o2, V o3)</code>: returns `o1[i] | o2[i] | o3[i]`.
    This is less efficient than `Xor3` on some targets; use that where possible.
*   <code>V **OrAnd**(V o, V a1, V a2)</code>: returns `o[i] | (a1[i] & a2[i])`.
*   <code>V **BitwiseIfThenElse**(V mask, V yes, V no)</code>: returns
    `((mask[i] & yes[i]) | (~mask[i] & no[i]))`. `BitwiseIfThenElse` is
    equivalent to, but potentially more efficient than `Or(And(mask, yes),
    AndNot(mask, no))`.

Special functions for signed types:

*   `V`: `{f}` \
    <code>V **CopySign**(V a, V b)</code>: returns the number with the magnitude
    of `a` and sign of `b`.

*   `V`: `{f}` \
    <code>V **CopySignToAbs**(V a, V b)</code>: as above, but potentially
    slightly more efficient; requires the first argument to be non-negative.

*   `V`: `{i}` \
    <code>V **BroadcastSignBit**(V a)</code> returns `a[i] < 0 ? -1 : 0`.

*   `V`: `{i,f}` \
    <code>V **ZeroIfNegative**(V v)</code>: returns `v[i] < 0 ? 0 : v[i]`.

*   `V`: `{i,f}` \
    <code>V **IfNegativeThenElse**(V v, V yes, V no)</code>: returns `v[i] < 0 ?
    yes[i] : no[i]`. This may be more efficient than `IfThenElse(Lt..)`.

*   `V`: `{i,f}` \
    <code>V **IfNegativeThenElseZero**(V v, V yes)</code>: returns
    `v[i] < 0 ? yes[i] : 0`. `IfNegativeThenElseZero(v, yes)` is equivalent to
    but more efficient than `IfThenElseZero(IsNegative(v), yes)` or
    `IfNegativeThenElse(v, yes, Zero(d))` on some targets.

*   `V`: `{i,f}` \
    <code>V **IfNegativeThenZeroElse**(V v, V no)</code>: returns
    `v[i] < 0 ? 0 : no`. `IfNegativeThenZeroElse(v, no)` is equivalent to
    but more efficient than `IfThenZeroElse(IsNegative(v), no)` or
    `IfNegativeThenElse(v, Zero(d), no)` on some targets.

*   `V`: `{i,f}` \
    <code>V **IfNegativeThenNegOrUndefIfZero**(V mask, V v)</code>: returns
    `mask[i] < 0 ? (-v[i]) : ((mask[i] > 0) ? v[i] : impl_defined_val)`, where
    `impl_defined_val` is an implementation-defined value that is equal to
    either 0 or `v[i]`.

    `IfNegativeThenNegOrUndefIfZero(mask, v)` is more efficient than
    `IfNegativeThenElse(mask, Neg(v), v)` for I8/I16/I32 vectors that are
    32 bytes or smaller on SSSE3/SSE4/AVX2/AVX3 targets.

### Masks

Let `M` denote a mask capable of storing a logical true/false for each lane (the
encoding depends on the platform).

#### Create mask

*   <code>M **FirstN**(D, size_t N)</code>: returns mask with the first `N`
    lanes (those with index `< N`) true. `N >= Lanes(D())` results in an
    all-true mask. `N` must not exceed
    `LimitsMax<SignedFromSize<HWY_MIN(sizeof(size_t), sizeof(TFromD<D>))>>()`.
    Useful for implementing "masked" stores by loading `prev` followed by
    `IfThenElse(FirstN(d, N), what_to_store, prev)`.

*   <code>M **MaskFromVec**(V v)</code>: returns false in lane `i` if `v[i] ==
    0`, or true if `v[i]` has all bits set. The result is
    *implementation-defined* if `v[i]` is neither zero nor all bits set.

*   <code>M **LoadMaskBits**(D, const uint8_t* p)</code>: returns a mask
    indicating whether the i-th bit in the array is set. Loads bytes and bits in
    ascending order of address and index. At least 8 bytes of `p` must be
    readable, but only `(Lanes(D()) + 7) / 8` need be initialized. Any unused
    bits (happens if `Lanes(D()) < 8`) are treated as if they were zero.

*   <code>M **Dup128MaskFromMaskBits**(D d, unsigned mask_bits)</code>: returns
    a mask with lane `i` set to
    `((mask_bits >> (i & (16 / sizeof(T) - 1))) & 1) != 0`.

*   <code>M **MaskFalse(D)**</code>: returns an all-false mask.
    `MaskFalse(D())` is equivalent to `MaskFromVec(Zero(D()))`, but
    `MaskFalse(D())` is more efficient than `MaskFromVec(Zero(D()))` on AVX3,
    RVV, and SVE.

    `MaskFalse(D())` is also equivalent to `FirstN(D(), 0)` or
    `Dup128MaskFromMaskBits(D(), 0)`, but `MaskFalse(D())` is usually more
    efficient.

*   <code>M **SetMask**(D, bool val)</code>: equivalent to
    `RebindMask(d, MaskFromVec(Set(RebindToSigned<D>(),
    -static_cast<MakeSigned<TFromD<D>>>(val))))`,
    but `SetMask(d, val)` is usually more efficient.

#### Convert mask

*   <code>M1 **RebindMask**(D, M2 m)</code>: returns same mask bits as `m`, but
    reinterpreted as a mask for lanes of type `TFromD<D>`. `M1` and `M2` must
    have the same number of lanes.

*   <code>V **VecFromMask**(D, M m)</code>: returns 0 in lane `i` if `m[i] ==
    false`, otherwise all bits set.

*   <code>uint64_t **BitsFromMask**(D, M m)</code>: returns bits `b` such that
    `(b >> i) & 1` indicates whether `m[i]` was set, and any remaining bits in
    the `uint64_t` are zero. This is only available if `HWY_MAX_BYTES <= 64`,
    because 512-bit vectors are the longest for which there are no more than 64
    lanes and thus mask bits.

*   <code>size_t **StoreMaskBits**(D, M m, uint8_t* p)</code>: stores a bit
    array indicating whether `m[i]` is true, in ascending order of `i`, filling
    the bits of each byte from least to most significant, then proceeding to the
    next byte. Returns the number of bytes written: `(Lanes(D()) + 7) / 8`. At
    least 8 bytes of `p` must be writable.

*   <code>Mask&lt;DTo&gt; **PromoteMaskTo**(DTo d_to, DFrom d_from,
    Mask&lt;DFrom&gt; m)</code>: Promotes `m` to a mask with a lane type of
    `TFromD<DTo>`, `DFrom` is `Rebind<TFrom, DTo>`.

    `PromoteMaskTo(d_to, d_from, m)` is equivalent to `MaskFromVec(BitCast(d_to,
    PromoteTo(di_to, BitCast(di_from, VecFromMask(d_from, m)))))`, where
    `di_from` is `RebindToSigned<DFrom>()` and `di_from` is
    `RebindToSigned<DFrom>()`, but `PromoteMaskTo(d_to, d_from, m)` is more
    efficient on some targets.

    PromoteMaskTo requires that `sizeof(TFromD<DFrom>) < sizeof(TFromD<DTo>)` be
    true.

*   <code>Mask&lt;DTo&gt; **DemoteMaskTo**(DTo d_to, DFrom d_from,
    Mask&lt;DFrom&gt; m)</code>: Demotes `m` to a mask with a lane type of
    `TFromD<DTo>`, `DFrom` is `Rebind<TFrom, DTo>`.

    `DemoteMaskTo(d_to, d_from, m)` is equivalent to `MaskFromVec(BitCast(d_to,
    DemoteTo(di_to, BitCast(di_from, VecFromMask(d_from, m)))))`, where
    `di_from` is `RebindToSigned<DFrom>()` and `di_from` is
    `RebindToSigned<DFrom>()`, but `DemoteMaskTo(d_to, d_from, m)` is more
    efficient on some targets.

    DemoteMaskTo requires that `sizeof(TFromD<DFrom>) > sizeof(TFromD<DTo>)` be
    true.

*   <code>M **OrderedDemote2MasksTo**(DTo, DFrom, M2, M2)</code>: returns a mask
    whose `LowerHalf` is the first argument and whose `UpperHalf` is the second
    argument; `M2` is `Mask<Half<DFrom>>`; `DTo` is `Repartition<TTo, DFrom>`.

    OrderedDemote2MasksTo requires that `sizeof(TFromD<DTo>) ==
    sizeof(TFromD<DFrom>) * 2` be true.

    `OrderedDemote2MasksTo(d_to, d_from, a, b)` is equivalent to
    `MaskFromVec(BitCast(d_to, OrderedDemote2To(di_to, va, vb)))`, where `va` is
    `BitCast(di_from, MaskFromVec(d_from, a))`, `vb` is `BitCast(di_from,
    MaskFromVec(d_from, b))`, `di_to` is `RebindToSigned<DTo>()`, and `di_from`
    is `RebindToSigned<DFrom>()`, but `OrderedDemote2MasksTo(d_to, d_from, a,
    b)` is more efficient on some targets.

    OrderedDemote2MasksTo is only available if `HWY_TARGET != HWY_SCALAR` is
    true.

#### Combine mask

*   <code>M2 **LowerHalfOfMask**(D d, M m)</code>:
    returns the lower half of mask `m`, where `M` is `MFromD<Twice<D>>`
    and `M2` is `MFromD<D>`.

    `LowerHalfOfMask(d, m)` is equivalent to
    `MaskFromVec(LowerHalf(d, VecFromMask(d, m)))`,
    but `LowerHalfOfMask(d, m)` is more efficient on some targets.

*   <code>M2 **UpperHalfOfMask**(D d, M m)</code>:
    returns the upper half of mask `m`, where `M` is `MFromD<Twice<D>>`
    and `M2` is `MFromD<D>`.

    `UpperHalfOfMask(d, m)` is equivalent to
    `MaskFromVec(UpperHalf(d, VecFromMask(d, m)))`,
    but `UpperHalfOfMask(d, m)` is more efficient on some targets.

    UpperHalfOfMask is only available if `HWY_TARGET != HWY_SCALAR` is true.

*   <code>M **CombineMasks**(D, M2, M2)</code>: returns a mask whose `UpperHalf`
    is the first argument and whose `LowerHalf` is the second argument; `M2` is
    `Mask<Half<D>>`.

    `CombineMasks(d, hi, lo)` is equivalent to `MaskFromVec(d, Combine(d,
    VecFromMask(Half<D>(), hi), VecFromMask(Half<D>(), lo)))`, but
    `CombineMasks(d, hi, lo)` is more efficient on some targets.

    CombineMasks is only available if `HWY_TARGET != HWY_SCALAR` is true.

#### Slide mask across blocks

*   <code>M **SlideMaskUpLanes**(D d, M m, size_t N)</code>:
    Slides `m` up `N` lanes. `SlideMaskUpLanes(d, m, N)` is equivalent to
    `MaskFromVec(SlideUpLanes(d, VecFromMask(d, m), N))`, but
    `SlideMaskUpLanes(d, m, N)` is more efficient on some targets.

    The results of SlideMaskUpLanes is implementation-defined if
    `N >= Lanes(d)`.

*   <code>M **SlideMaskDownLanes**(D d, M m, size_t N)</code>:
    Slides `m` down `N` lanes. `SlideMaskDownLanes(d, m, N)` is equivalent to
    `MaskFromVec(SlideDownLanes(d, VecFromMask(d, m), N))`, but
    `SlideMaskDownLanes(d, m, N)` is more efficient on some targets.

    The results of SlideMaskDownLanes is implementation-defined if
    `N >= Lanes(d)`.

*   <code>M **SlideMask1Up**(D d, M m)</code>:
    Slides `m` up 1 lane. `SlideMask1Up(d, m)` is equivalent to
    `MaskFromVec(Slide1Up(d, VecFromMask(d, m)))`, but `SlideMask1Up(d, m)` is
    more efficient on some targets.

*   <code>M **SlideMask1Down**(D d, M m)</code>:
    Slides `m` down 1 lane. `SlideMask1Down(d, m)` is equivalent to
    `MaskFromVec(Slide1Down(d, VecFromMask(d, m)))`, but `SlideMask1Down(d, m)` is
    more efficient on some targets.

#### Test mask

*   <code>bool **AllTrue**(D, M m)</code>: returns whether all `m[i]` are true.

*   <code>bool **AllFalse**(D, M m)</code>: returns whether all `m[i]` are
    false.

*   <code>size_t **CountTrue**(D, M m)</code>: returns how many of `m[i]` are
    true [0, N]. This is typically more expensive than AllTrue/False.

*   <code>intptr_t **FindFirstTrue**(D, M m)</code>: returns the index of the
    first (i.e. lowest index) `m[i]` that is true, or -1 if none are.

*   <code>size_t **FindKnownFirstTrue**(D, M m)</code>: returns the index of the
    first (i.e. lowest index) `m[i]` that is true. Requires `!AllFalse(d, m)`,
    otherwise results are undefined. This is typically more efficient than
    `FindFirstTrue`.

*   <code>intptr_t **FindLastTrue**(D, M m)</code>: returns the index of the
    last (i.e. highest index) `m[i]` that is true, or -1 if none are.

*   <code>size_t **FindKnownLastTrue**(D, M m)</code>: returns the index of the
    last (i.e. highest index) `m[i]` that is true. Requires `!AllFalse(d, m)`,
    otherwise results are undefined. This is typically more efficient than
    `FindLastTrue`.

#### Ternary operator for masks

For `IfThen*`, masks must adhere to the invariant established by `MaskFromVec`:
false is zero, true has all bits set:

*   <code>V **IfThenElse**(M mask, V yes, V no)</code>: returns `mask[i] ?
    yes[i] : no[i]`.

*   <code>V **IfThenElseZero**(M mask, V yes)</code>: returns `mask[i] ?
    yes[i] : 0`.

*   <code>V **IfThenZeroElse**(M mask, V no)</code>: returns `mask[i] ? 0 :
    no[i]`.

*   <code>V **IfVecThenElse**(V mask, V yes, V no)</code>: equivalent to and
    possibly faster than `IfVecThenElse(MaskFromVec(mask), yes, no)`. The result
    is *implementation-defined* if `mask[i]` is neither zero nor all bits set.

#### Logical mask

*   <code>M **Not**(M m)</code>: returns mask of elements indicating whether the
    input mask element was false.

*   <code>M **And**(M a, M b)</code>: returns mask of elements indicating
    whether both input mask elements were true.

*   <code>M **AndNot**(M not_a, M b)</code>: returns mask of elements indicating
    whether `not_a` is false and `b` is true.

*   <code>M **Or**(M a, M b)</code>: returns mask of elements indicating whether
    either input mask element was true.

*   <code>M **Xor**(M a, M b)</code>: returns mask of elements indicating
    whether exactly one input mask element was true.

*   <code>M **ExclusiveNeither**(M a, M b)</code>: returns mask of elements
    indicating `a` is false and `b` is false. Undefined if both are true. We
    choose not to provide NotOr/NotXor because x86 and SVE only define one of
    these operations. This op is for situations where the inputs are known to be
    mutually exclusive.

*   <code>M **SetOnlyFirst**(M m)</code>: If none of `m[i]` are true, returns
    all-false. Otherwise, only lane `k` is true, where `k` is equal to
    `FindKnownFirstTrue(m)`. In other words, sets to false any lanes with index
    greater than the first true lane, if it exists.

*   <code>M **SetBeforeFirst**(M m)</code>: If none of `m[i]` are true, returns
    all-true. Otherwise, returns mask with the first `k` lanes true and all
    remaining lanes false, where `k` is equal to `FindKnownFirstTrue(m)`. In
    other words, if at least one of `m[i]` is true, sets to true any lanes with
    index less than the first true lane and all remaining lanes to false.

*   <code>M **SetAtOrBeforeFirst**(M m)</code>: equivalent to
    `Or(SetBeforeFirst(m), SetOnlyFirst(m))`, but `SetAtOrBeforeFirst(m)` is
    usually more efficient than `Or(SetBeforeFirst(m), SetOnlyFirst(m))`.

*   <code>M **SetAtOrAfterFirst**(M m)</code>: equivalent to
    `Not(SetBeforeFirst(m))`.

#### Compress

*   <code>V **Compress**(V v, M m)</code>: returns `r` such that `r[n]` is
    `v[i]`, with `i` the n-th lane index (starting from 0) where `m[i]` is true.
    Compacts lanes whose mask is true into the lower lanes. For targets and lane
    type `T` where `CompressIsPartition<T>::value` is true, the upper lanes are
    those whose mask is false (thus `Compress` corresponds to partitioning
    according to the mask). Otherwise, the upper lanes are
    implementation-defined. Potentially slow with 8 and 16-bit lanes. Use this
    form when the input is already a mask, e.g. returned by a comparison.

*   <code>V **CompressNot**(V v, M m)</code>: equivalent to `Compress(v,
    Not(m))` but possibly faster if `CompressIsPartition<T>::value` is true.

*   `V`: `u64` \
    <code>V **CompressBlocksNot**(V v, M m)</code>: equivalent to
    `CompressNot(v, m)` when `m` is structured as adjacent pairs (both true or
    false), e.g. as returned by `Lt128`. This is a no-op for 128 bit vectors.
    Unavailable if `HWY_TARGET == HWY_SCALAR`.

*   <code>size_t **CompressStore**(V v, M m, D d, T* p)</code>: writes lanes
    whose mask `m` is true into `p`, starting from lane 0. Returns `CountTrue(d,
    m)`, the number of valid lanes. May be implemented as `Compress` followed by
    `StoreU`; lanes after the valid ones may still be overwritten! Potentially
    slow with 8 and 16-bit lanes.

*   <code>size_t **CompressBlendedStore**(V v, M m, D d, T* p)</code>: writes
    only lanes whose mask `m` is true into `p`, starting from lane 0. Returns
    `CountTrue(d, m)`, the number of lanes written. Does not modify subsequent
    lanes, but there is no guarantee of atomicity because this may be
    implemented as `Compress, LoadU, IfThenElse(FirstN), StoreU`.

*   <code>V **CompressBits**(V v, const uint8_t* HWY_RESTRICT bits)</code>:
    Equivalent to, but often faster than `Compress(v, LoadMaskBits(d, bits))`.
    `bits` is as specified for `LoadMaskBits`. If called multiple times, the
    `bits` pointer passed to this function must also be marked `HWY_RESTRICT` to
    avoid repeated work. Note that if the vector has less than 8 elements,
    incrementing `bits` will not work as intended for packed bit arrays. As with
    `Compress`, `CompressIsPartition` indicates the mask=false lanes are moved
    to the upper lanes. Potentially slow with 8 and 16-bit lanes.

*   <code>size_t **CompressBitsStore**(V v, const uint8_t* HWY_RESTRICT bits, D
    d, T* p)</code>: combination of `CompressStore` and `CompressBits`, see
    remarks there.

#### Expand

*   <code>V **Expand**(V v, M m)</code>: returns `r` such that `r[i]` is zero
    where `m[i]` is false, and otherwise `v[s]`, where `s` is the number of
    `m[0, i)` which are true. Scatters inputs in ascending index order to the
    lanes whose mask is true and zeros all other lanes. Potentially slow with 8
    and 16-bit lanes.

*   <code>V **LoadExpand**(M m, D d, const T* p)</code>: returns `r` such that
    `r[i]` is zero where `m[i]` is false, and otherwise `p[s]`, where `s` is the
    number of `m[0, i)` which are true. May be implemented as `LoadU` followed
    by `Expand`. Potentially slow with 8 and 16-bit lanes.

### Comparisons

These return a mask (see above) indicating whether the condition is true.

*   <code>M **operator==**(V a, V b)</code>: returns `a[i] == b[i]`. Currently
    unavailable on SVE/RVV; use the equivalent `Eq` instead.
*   <code>M **operator!=**(V a, V b)</code>: returns `a[i] != b[i]`. Currently
    unavailable on SVE/RVV; use the equivalent `Ne` instead.

*   <code>M **operator&lt;**(V a, V b)</code>: returns `a[i] < b[i]`. Currently
    unavailable on SVE/RVV; use the equivalent `Lt` instead.

*   <code>M **operator&gt;**(V a, V b)</code>: returns `a[i] > b[i]`. Currently
    unavailable on SVE/RVV; use the equivalent `Gt` instead.

*   <code>M **operator&lt;=**(V a, V b)</code>: returns `a[i] <= b[i]`.
    Currently unavailable on SVE/RVV; use the equivalent `Le` instead.

*   <code>M **operator&gt;=**(V a, V b)</code>: returns `a[i] >= b[i]`.
    Currently unavailable on SVE/RVV; use the equivalent `Ge` instead.

*   `V`: `{i,f}` \
    <code>M **IsNegative**(V v)</code>: returns `v[i] < 0`.

    `IsNegative(v)` is equivalent to `MaskFromVec(BroadcastSignBit(v))` or
    `Lt(v, Zero(d))`, but `IsNegative(v)` is more efficient on some targets.

*   `V`: `{u,i}` \
    <code>M **TestBit**(V v, V bit)</code>: returns `(v[i] & bit[i]) == bit[i]`.
    `bit[i]` must have exactly one bit set.

*   `V`: `u64` \
    <code>M **Lt128**(D, V a, V b)</code>: for each adjacent pair of 64-bit
    lanes (e.g. indices 1,0), returns whether `a[1]:a[0]` concatenated to an
    unsigned 128-bit integer (least significant bits in `a[0]`) is less than
    `b[1]:b[0]`. For each pair, the mask lanes are either both true or both
    false. Unavailable if `HWY_TARGET == HWY_SCALAR`.

*   `V`: `u64` \
    <code>M **Lt128Upper**(D, V a, V b)</code>: for each adjacent pair of 64-bit
    lanes (e.g. indices 1,0), returns whether `a[1]` is less than `b[1]`. For
    each pair, the mask lanes are either both true or both false. This is useful
    for comparing 64-bit keys alongside 64-bit values. Only available if
    `HWY_TARGET != HWY_SCALAR`.

*   `V`: `u64` \
    <code>M **Eq128**(D, V a, V b)</code>: for each adjacent pair of 64-bit
    lanes (e.g. indices 1,0), returns whether `a[1]:a[0]` concatenated to an
    unsigned 128-bit integer (least significant bits in `a[0]`) equals
    `b[1]:b[0]`. For each pair, the mask lanes are either both true or both
    false. Unavailable if `HWY_TARGET == HWY_SCALAR`.

*   `V`: `u64` \
    <code>M **Ne128**(D, V a, V b)</code>: for each adjacent pair of 64-bit
    lanes (e.g. indices 1,0), returns whether `a[1]:a[0]` concatenated to an
    unsigned 128-bit integer (least significant bits in `a[0]`) differs from
    `b[1]:b[0]`. For each pair, the mask lanes are either both true or both
    false. Unavailable if `HWY_TARGET == HWY_SCALAR`.

*   `V`: `u64` \
    <code>M **Eq128Upper**(D, V a, V b)</code>: for each adjacent pair of 64-bit
    lanes (e.g. indices 1,0), returns whether `a[1]` equals `b[1]`. For each
    pair, the mask lanes are either both true or both false. This is useful for
    comparing 64-bit keys alongside 64-bit values. Only available if `HWY_TARGET
    != HWY_SCALAR`.

*   `V`: `u64` \
    <code>M **Ne128Upper**(D, V a, V b)</code>: for each adjacent pair of 64-bit
    lanes (e.g. indices 1,0), returns whether `a[1]` differs from `b[1]`. For
    each pair, the mask lanes are either both true or both false. This is useful
    for comparing 64-bit keys alongside 64-bit values. Only available if
    `HWY_TARGET != HWY_SCALAR`.

#### Masked comparison

All ops in this section return `false` for `mask=false` lanes. These are
equivalent to, and potentially more efficient than, `And(m, Eq(a, b));` etc.

*   <code>M **MaskedEq**(M m, V a, V b)</code>: returns `a[i] == b[i]` or
    `false` if `m[i]` is false.

*   <code>M **MaskedNe**(M m, V a, V b)</code>: returns `a[i] != b[i]` or
    `false` if `m[i]` is false.

*   <code>M **MaskedLt**(M m, V a, V b)</code>: returns `a[i] < b[i]` or
    `false` if `m[i]` is false.

*   <code>M **MaskedGt**(M m, V a, V b)</code>: returns `a[i] > b[i]` or
    `false` if `m[i]` is false.

*   <code>M **MaskedLe**(M m, V a, V b)</code>: returns `a[i] <= b[i]` or
    `false` if `m[i]` is false.

*   <code>M **MaskedGe**(M m, V a, V b)</code>: returns `a[i] >= b[i]` or
    `false` if `m[i]` is false.

### Memory

Memory operands are little-endian, otherwise their order would depend on the
lane configuration. Pointers are the addresses of `N` consecutive `T` values,
either `aligned` (address is a multiple of the vector size) or possibly
unaligned (denoted `p`).

Even unaligned addresses must still be a multiple of `sizeof(T)`, otherwise
`StoreU` may crash on some platforms (e.g. RVV and Armv7). Note that C++ ensures
automatic (stack) and dynamically allocated (via `new` or `malloc`) variables of
type `T` are aligned to `sizeof(T)`, hence such addresses are suitable for
`StoreU`. However, casting pointers to `char*` and adding arbitrary offsets (not
a multiple of `sizeof(T)`) can violate this requirement.

**Note**: computations with low arithmetic intensity (FLOP/s per memory traffic
bytes), e.g. dot product, can be *1.5 times as fast* when the memory operands
are aligned to the vector size. An unaligned access may require two load ports.

#### Load

*   <code>Vec&lt;D&gt; **Load**(D, const T* aligned)</code>: returns
    `aligned[i]`. May fault if the pointer is not aligned to the vector size
    (using aligned_allocator.h is safe). Using this whenever possible improves
    codegen on SSSE3/SSE4: unlike `LoadU`, `Load` can be fused into a memory
    operand, which reduces register pressure.

Requires only *element-aligned* vectors (e.g. from malloc/std::vector, or
aligned memory at indices which are not a multiple of the vector length):

*   <code>Vec&lt;D&gt; **LoadU**(D, const T* p)</code>: returns `p[i]`.

*   <code>Vec&lt;D&gt; **LoadDup128**(D, const T* p)</code>: returns one 128-bit
    block loaded from `p` and broadcasted into all 128-bit block\[s\]. This may
    be faster than broadcasting single values, and is more convenient than
    preparing constants for the actual vector length. Only available if
    `HWY_TARGET != HWY_SCALAR`.

*   <code>Vec&lt;D&gt; **MaskedLoadOr**(V no, M mask, D, const T* p)</code>:
    returns `mask[i] ? p[i] : no[i]`. May fault even where `mask` is false `#if
    HWY_MEM_OPS_MIGHT_FAULT`. If `p` is aligned, faults cannot happen unless the
    entire vector is inaccessible. Assuming no faults, this is equivalent to,
    and potentially more efficient than, `IfThenElse(mask, LoadU(D(), p), no)`.

*   <code>Vec&lt;D&gt; **MaskedLoad**(M mask, D d, const T* p)</code>:
    equivalent to `MaskedLoadOr(Zero(d), mask, d, p)`, but potentially slightly
    more efficient.

*   <code>Vec&lt;D&gt; **LoadN**(D d, const T* p, size_t max_lanes_to_load)
    </code>: Loads `HWY_MIN(Lanes(d), max_lanes_to_load)` lanes from `p` to the
    first (lowest-index) lanes of the result vector and zeroes out the remaining
    lanes.

    LoadN does not fault if all of the elements in `[p, p + max_lanes_to_load)`
    are accessible, even if `HWY_MEM_OPS_MIGHT_FAULT` is 1 or `max_lanes_to_load
    < Lanes(d)` is true.

*   <code>Vec&lt;D&gt; **LoadNOr**(V no, D d, const T* p, size_t
    max_lanes_to_load) </code>: Loads `HWY_MIN(Lanes(d), max_lanes_to_load)`
    lanes from `p` to the first (lowest-index) lanes of the result vector and
    fills the remaining lanes with `no`. Like LoadN, this does not fault.

*   <code> Vec&lt;D&gt; **InsertIntoUpper**(D d, T* p, V v)</code>: Loads
    `Lanes(d)/2` lanes from `p` into the upper lanes of the result vector and
    the lower half of `v` into the lower lanes.

#### Store

*   <code>void **Store**(Vec&lt;D&gt; v, D, T* aligned)</code>: copies `v[i]`
    into `aligned[i]`, which must be aligned to the vector size. Writes exactly
    `N * sizeof(T)` bytes.

*   <code>void **StoreU**(Vec&lt;D&gt; v, D, T* p)</code>: as `Store`, but the
    alignment requirement is relaxed to element-aligned (multiple of
    `sizeof(T)`).

*   <code>void **BlendedStore**(Vec&lt;D&gt; v, M m, D d, T* p)</code>: as
    `StoreU`, but only updates `p` where `m` is true. May fault even where
    `mask` is false `#if HWY_MEM_OPS_MIGHT_FAULT`. If `p` is aligned, faults
    cannot happen unless the entire vector is inaccessible. Equivalent to, and
    potentially more efficient than, `StoreU(IfThenElse(m, v, LoadU(d, p)), d,
    p)`. "Blended" indicates this may not be atomic; other threads must not
    concurrently update `[p, p + Lanes(d))` without synchronization.

*   <code>void **SafeFillN**(size_t num, T value, D d, T* HWY_RESTRICT
    to)</code>: Sets `to[0, num)` to `value`. If `num` exceeds `Lanes(d)`, the
    behavior is target-dependent (either filling all, or no more than one
    vector). Potentially more efficient than a scalar loop, but will not fault,
    unlike `BlendedStore`. No alignment requirement. Potentially non-atomic,
    like `BlendedStore`.

*   <code>void **SafeCopyN**(size_t num, D d, const T* HWY_RESTRICT from, T*
    HWY_RESTRICT to)</code>: Copies `from[0, num)` to `to`. If `num` exceeds
    `Lanes(d)`, the behavior is target-dependent (either copying all, or no more
    than one vector). Potentially more efficient than a scalar loop, but will
    not fault, unlike `BlendedStore`. No alignment requirement. Potentially
    non-atomic, like `BlendedStore`.

*   <code>void **StoreN**(Vec&lt;D&gt; v, D d, T* HWY_RESTRICT p,
    size_t max_lanes_to_store)</code>: Stores the first (lowest-index)
    `HWY_MIN(Lanes(d), max_lanes_to_store)` lanes of `v` to p.

    StoreN does not modify any memory past
    `p + HWY_MIN(Lanes(d), max_lanes_to_store) - 1`.

*   <code>void **TruncateStore**(Vec&lt;D&gt; v, D d, T* HWY_RESTRICT p)</code>:
    Truncates elements of `v` to type `T` and stores on `p`. It is similar to
    performing `TruncateTo` followed by `StoreU`.

#### Interleaved

*   <code>void **LoadInterleaved2**(D, const T* p, Vec&lt;D&gt;&amp; v0,
    Vec&lt;D&gt;&amp; v1)</code>: equivalent to `LoadU` into `v0, v1` followed
    by shuffling, such that `v0[0] == p[0], v1[0] == p[1]`.

*   <code>void **LoadInterleaved3**(D, const T* p, Vec&lt;D&gt;&amp; v0,
    Vec&lt;D&gt;&amp; v1, Vec&lt;D&gt;&amp; v2)</code>: as above, but for three
    vectors (e.g. RGB samples).

*   <code>void **LoadInterleaved4**(D, const T* p, Vec&lt;D&gt;&amp; v0,
    Vec&lt;D&gt;&amp; v1, Vec&lt;D&gt;&amp; v2, Vec&lt;D&gt;&amp; v3)</code>: as
    above, but for four vectors (e.g. RGBA).

*   <code>void **StoreInterleaved2**(Vec&lt;D&gt; v0, Vec&lt;D&gt; v1, D, T*
    p)</code>: equivalent to shuffling `v0, v1` followed by two `StoreU()`, such
    that `p[0] == v0[0], p[1] == v1[0]`.

*   <code>void **StoreInterleaved3**(Vec&lt;D&gt; v0, Vec&lt;D&gt; v1,
    Vec&lt;D&gt; v2, D, T* p)</code>: as above, but for three vectors (e.g. RGB
    samples).

*   <code>void **StoreInterleaved4**(Vec&lt;D&gt; v0, Vec&lt;D&gt; v1,
    Vec&lt;D&gt; v2, Vec&lt;D&gt; v3, D, T* p)</code>: as above, but for four
    vectors (e.g. RGBA samples).

#### Scatter/Gather

**Note**: Offsets/indices are of type `VI = Vec<RebindToSigned<D>>` and need not
be unique. The results are implementation-defined for negative offsets, because
behavior differs between x86 and RVV (signed vs. unsigned).

**Note**: Where possible, applications should `Load/Store/TableLookup*` entire
vectors, which is much faster than `Scatter/Gather`. Otherwise, code of the form
`dst[tbl[i]] = F(src[i])` should when possible be transformed to `dst[i] =
F(src[tbl[i]])` because `Scatter` may be more expensive than `Gather`.

**Note**: We provide `*Offset` functions for the convenience of users that have
actual byte offsets. However, the preferred interface is `*Index`, which takes
indices. To reduce the number of ops, we do not intend to add `Masked*` ops for
offsets. If you have offsets, you can convert them to indices via `ShiftRight`.

*   `D`: `{u,i,f}{32,64}` \
    <code>void **ScatterOffset**(Vec&lt;D&gt; v, D, T* base, VI offsets)</code>:
    stores `v[i]` to the base address plus *byte* `offsets[i]`.

*   `D`: `{u,i,f}{32,64}` \
    <code>void **ScatterIndex**(Vec&lt;D&gt; v, D, T* base, VI indices)</code>:
    stores `v[i]` to `base[indices[i]]`.

*   `D`: `{u,i,f}{32,64}` \
    <code>void **ScatterIndexN**(Vec&lt;D&gt; v, D, T* base, VI indices, size_t
    max_lanes_to_store)</code>: Stores `HWY_MIN(Lanes(d), max_lanes_to_store)`
    lanes `v[i]` to `base[indices[i]]`

*   `D`: `{u,i,f}{32,64}` \
    <code>void **MaskedScatterIndex**(Vec&lt;D&gt; v, M m, D, T* base, VI
    indices)</code>: stores `v[i]` to `base[indices[i]]` if `mask[i]` is true.
    Does not fault for lanes whose `mask` is false.

*   `D`: `{u,i,f}{32,64}` \
    <code>Vec&lt;D&gt; **GatherOffset**(D, const T* base, VI offsets)</code>:
    returns elements of base selected by *byte* `offsets[i]`.

*   `D`: `{u,i,f}{32,64}` \
    <code>Vec&lt;D&gt; **GatherIndex**(D, const T* base, VI indices)</code>:
    returns vector of `base[indices[i]]`.

*   `D`: `{u,i,f}{32,64}` \
    <code>Vec&lt;D&gt; **GatherIndexN**(D, const T* base, VI indices, size_t
    max_lanes_to_load)</code>: Loads `HWY_MIN(Lanes(d), max_lanes_to_load)`
    lanes of `base[indices[i]]` to the first (lowest-index) lanes of the result
    vector and zeroes out the remaining lanes.

*   `D`: `{u,i,f}{32,64}` \
    <code>Vec&lt;D&gt; **MaskedGatherIndexOr**(V no, M mask, D d, const T* base,
    VI indices)</code>: returns vector of `base[indices[i]]` where `mask[i]` is
    true, otherwise `no[i]`. Does not fault for lanes whose `mask` is false.
    This is equivalent to, and potentially more efficient than,
    `IfThenElseZero(mask, GatherIndex(d, base, indices))`.

*   `D`: `{u,i,f}{32,64}` \
    <code>Vec&lt;D&gt; **MaskedGatherIndex**(M mask, D d, const T* base, VI
    indices)</code>: equivalent to `MaskedGatherIndexOr(Zero(d), mask, d, base,
    indices)`. Use this when the desired default value is zero; it may be more
    efficient on some targets, and on others require generating a zero constant.

### Cache control

All functions except `Stream` are defined in cache_control.h.

*   <code>void **Stream**(Vec&lt;D&gt; a, D d, const T* aligned)</code>: copies
    `a[i]` into `aligned[i]` with non-temporal hint if available (useful for
    write-only data; avoids cache pollution). May be implemented using a
    CPU-internal buffer. To avoid partial flushes and unpredictable interactions
    with atomics (for example, see Intel SDM Vol 4, Sec. 8.1.2.2), call this
    consecutively for an entire cache line (typically 64 bytes, aligned to its
    size). Each call may write a multiple of `HWY_STREAM_MULTIPLE` bytes, which
    can exceed `Lanes(d) * sizeof(T)`. The new contents of `aligned` may not be
    visible until `FlushStream` is called.

*   <code>void **FlushStream**()</code>: ensures values written by previous
    `Stream` calls are visible on the current core. This is NOT sufficient for
    synchronizing across cores; when `Stream` outputs are to be consumed by
    other core(s), the producer must publish availability (e.g. via mutex or
    atomic_flag) after `FlushStream`.

*   <code>void **FlushCacheline**(const void* p)</code>: invalidates and flushes
    the cache line containing "p", if possible.

*   <code>void **Prefetch**(const T* p)</code>: optionally begins loading the
    cache line containing "p" to reduce latency of subsequent actual loads.

*   <code>void **Pause**()</code>: when called inside a spin-loop, may reduce
    power consumption.

### Type conversion

*   <code>Vec&lt;D&gt; **BitCast**(D, V)</code>: returns the bits of `V`
    reinterpreted as type `Vec<D>`.

*   <code>Vec&lt;D&gt; **ResizeBitCast**(D, V)</code>: resizes `V` to a vector
    of `Lanes(D()) * sizeof(TFromD<D>)` bytes, and then returns the bits of the
    resized vector reinterpreted as type `Vec<D>`.

    If `Vec<D>` is a larger vector than `V`, then the contents of any bytes past
    the first `Lanes(DFromV<V>()) * sizeof(TFromV<V>)` bytes of the result
    vector is unspecified.

*   <code>Vec&lt;DTo&gt; **ZeroExtendResizeBitCast**(DTo, DFrom, V)</code>:
    resizes `V`, which is a vector of type `Vec<DFrom>`, to a vector of
    `Lanes(D()) * sizeof(TFromD<D>)` bytes, and then returns the bits of the
    resized vector reinterpreted as type `Vec<DTo>`.

    If `Lanes(DTo()) * sizeof(TFromD<DTo>)` is greater than `Lanes(DFrom()) *
    sizeof(TFromD<DFrom>)`, then any bytes past the first `Lanes(DFrom()) *
    sizeof(TFromD<DFrom>)` bytes of the result vector are zeroed out.

*   `V`,`V8`: (`u32,u8`) \
    <code>V8 **U8FromU32**(V)</code>: special-case `u32` to `u8` conversion when
    all lanes of `V` are already clamped to `[0, 256)`.

*   `D`: `{f}` \
    <code>Vec&lt;D&gt; **ConvertTo**(D, V)</code>: converts a signed/unsigned
    integer value to same-sized floating point.

*   `V`: `{f}` \
    <code>Vec&lt;D&gt; **ConvertTo**(D, V)</code>: rounds floating point towards
    zero and converts the value to same-sized signed/unsigned integer. Returns
    the closest representable value if the input exceeds the destination range.

*   `V`: `{f}` \
    <code>Vec&lt;D&gt; **ConvertInRangeTo**(D, V)</code>: rounds floating point
    towards zero and converts the value to same-sized signed/unsigned integer.
    Returns an implementation-defined value if the input exceeds the destination
    range.

*   `V`: `f`; `Ret`: `Vec<RebindToSigned<DFromV<V>>>` \
    <code>Ret **NearestInt**(V a)</code>: returns the integer nearest to `a[i]`;
    results are undefined for NaN.

*   `V`: `f`; `Ret`: `Vec<RebindToSigned<DFromV<V>>>` \
    <code>Ret **CeilInt**(V a)</code>: equivalent to
    `ConvertTo(RebindToSigned<DFromV<V>>(), Ceil(a))`, but `CeilInt(a)` is more
    efficient on some targets, including SSE2, SSSE3, and AArch64 NEON.

*   `V`: `f`; `Ret`: `Vec<RebindToSigned<DFromV<V>>>` \
    <code>Ret **FloorInt**(V a)</code>: equivalent to
    `ConvertTo(RebindToSigned<DFromV<V>>(), Floor(a))`, but `FloorInt(a)` is
    more efficient on some targets, including SSE2, SSSE3, and AArch64 NEON.

*   `D`: `i32`, `V`: `f64`
    <code>Vec&lt;D&gt; **DemoteToNearestInt**(D d, V v)</code>: converts `v[i]`
    to `TFromD<D>`, rounding to nearest (with ties to even).

    `DemoteToNearestInt(d, v)` is equivalent to `DemoteTo(d, Round(v))`, but
    `DemoteToNearestInt(d, v)` is more efficient on some targets, including x86
    and RVV.

*   <code>Vec&lt;D&gt; **MaskedConvertTo**(M m, D d, V v)</code>: returns `v[i]`
    converted to `D` where m is active and returns zero otherwise.

#### Single vector demotion

These functions demote a full vector (or parts thereof) into a vector of half
the size. Use `Rebind<MakeNarrow<T>, D>` or `Half<RepartitionToNarrow<D>>` to
obtain the `D` that describes the return type.

*   `V`,`D`: (`u64,u32`), (`u64,u16`), (`u64,u8`), (`u32,u16`), (`u32,u8`),
    (`u16,u8`) \
    <code>Vec&lt;D&gt; **TruncateTo**(D, V v)</code>: returns `v[i]` truncated
    to the smaller type indicated by `T = TFromD<D>`, with the same result as if
    the more-significant input bits that do not fit in `T` had been zero.
    Example: `ScalableTag<uint32_t> du32; Rebind<uint8_t> du8; TruncateTo(du8,
    Set(du32, 0xF08F))` is the same as `Set(du8, 0x8F)`.

*   `V`,`D`: (`i16,i8`), (`i32,i8`), (`i64,i8`), (`i32,i16`), (`i64,i16`),
    (`i64,i32`), (`u16,i8`), (`u32,i8`), (`u64,i8`), (`u32,i16`), (`u64,i16`),
    (`u64,i32`), (`i16,u8`), (`i32,u8`), (`i64,u8`), (`i32,u16`), (`i64,u16`),
    (`i64,u32`), (`u16,u8`), (`u32,u8`), (`u64,u8`), (`u32,u16`), (`u64,u16`),
    (`u64,u32`), (`f64,f32`) \
    <code>Vec&lt;D&gt; **DemoteTo**(D, V v)</code>: returns `v[i]` after packing
    with signed/unsigned saturation to `MakeNarrow<T>`.

*   `V`,`D`: `f64,{u,i}32` \
    <code>Vec&lt;D&gt; **DemoteTo**(D, V v)</code>: rounds floating point
    towards zero and converts the value to 32-bit integers. Returns the closest
    representable value if the input exceeds the destination range.

*   `V`,`D`: `f64,{u,i}32` \
    <code>Vec&lt;D&gt; **DemoteInRangeTo**(D, V v)</code>: rounds floating point
    towards zero and converts the value to 32-bit integers. Returns an
    implementation-defined value if the input exceeds the destination range.

*   `V`,`D`: `{u,i}64,f32` \
    <code>Vec&lt;D&gt; **DemoteTo**(D, V v)</code>: converts 64-bit integer to
    `float`.

*   `V`,`D`: (`f32,f16`), (`f64,f16`), (`f32,bf16`) \
    <code>Vec&lt;D&gt; **DemoteTo**(D, V v)</code>: narrows float to half (for
    bf16, it is unspecified whether this truncates or rounds).

#### Single vector promotion

These functions promote a half vector to a full vector. To obtain halves, use
`LowerHalf` or `UpperHalf`, or load them using a half-sized `D`.

*   Unsigned `V` to wider signed/unsigned `D`; signed to wider signed, `f16` to
    `f32`, `f16` to `f64`, `bf16` to `f32`, `f32` to `f64` \
    <code>Vec&lt;D&gt; **PromoteTo**(D, V part)</code>: returns `part[i]` zero-
    or sign-extended to the integer type `MakeWide<T>`, or widened to the
    floating-point type `MakeFloat<MakeWide<T>>`.

*   `{u,i}32` to `f64` \
    <code>Vec&lt;D&gt; **PromoteTo**(D, V part)</code>: returns `part[i]`
    widened to `double`.

*   `f32` to `i64` or `u64` \
    <code>Vec&lt;D&gt; **PromoteTo**(D, V part)</code>: rounds `part[i]` towards
    zero and converts the rounded value to a 64-bit signed or unsigned integer.
    Returns the representable value if the input exceeds the destination range.

*   `f32` to `i64` or `u64` \
    <code>Vec&lt;D&gt; **PromoteInRangeTo**(D, V part)</code>: rounds `part[i]`
    towards zero and converts the rounded value to a 64-bit signed or unsigned
    integer. Returns an implementation-defined value if the input exceeds the
    destination range.

The following may be more convenient or efficient than also calling `LowerHalf`
/ `UpperHalf`:

*   Unsigned `V` to wider signed/unsigned `D`; signed to wider signed, `f16` to
    `f32`, `bf16` to `f32`, `f32` to `f64` \
    <code>Vec&lt;D&gt; **PromoteLowerTo**(D, V v)</code>: returns `v[i]` widened
    to `MakeWide<T>`, for i in `[0, Lanes(D()))`. Note that `V` has twice as
    many lanes as `D` and the return value.

*   `{u,i}32` to `f64` \
    <code>Vec&lt;D&gt; **PromoteLowerTo**(D, V v)</code>: returns `v[i]` widened
    to `double`, for i in `[0, Lanes(D()))`. Note that `V` has twice as many
    lanes as `D` and the return value.

*   `f32` to `i64` or `u64` \
    <code>Vec&lt;D&gt; **PromoteLowerTo**(D, V v)</code>: rounds `v[i]` towards
    zero and converts the rounded value to a 64-bit signed or unsigned integer,
    for i in `[0, Lanes(D()))`. Note that `V` has twice as many lanes as `D` and
    the return value.

*   `f32` to `i64` or `u64` \
    <code>Vec&lt;D&gt; **PromoteInRangeLowerTo**(D, V v)</code>: rounds `v[i]`
    towards zero and converts the rounded value to a 64-bit signed or unsigned
    integer, for i in `[0, Lanes(D()))`. Note that `V` has twice as many lanes
    as `D` and the return value. Returns an implementation-defined value if the
    input exceeds the destination range.

*   Unsigned `V` to wider signed/unsigned `D`; signed to wider signed, `f16` to
    `f32`, `bf16` to `f32`, `f32` to `f64` \
    <code>Vec&lt;D&gt; **PromoteUpperTo**(D, V v)</code>: returns `v[i]` widened
    to `MakeWide<T>`, for i in `[Lanes(D()), 2 * Lanes(D()))`. Note that `V` has
    twice as many lanes as `D` and the return value. Only available if
    `HWY_TARGET != HWY_SCALAR`.

*   `{u,i}32` to `f64` \
    <code>Vec&lt;D&gt; **PromoteUpperTo**(D, V v)</code>: returns `v[i]` widened
    to `double`, for i in `[Lanes(D()), 2 * Lanes(D()))`. Note that `V` has
    twice as many lanes as `D` and the return value. Only available if
    `HWY_TARGET != HWY_SCALAR`.

*   `f32` to `i64` or `u64` \
    <code>Vec&lt;D&gt; **PromoteUpperTo**(D, V v)</code>: rounds `v[i]` towards
    zero and converts the rounded value to a 64-bit signed or unsigned integer,
    for i in `[Lanes(D()), 2 * Lanes(D()))`. Note that `V` has twice as many
    lanes as `D` and the return value. Only available if
    `HWY_TARGET != HWY_SCALAR`.

*   `f32` to `i64` or `u64` \
    <code>Vec&lt;D&gt; **PromoteInRangeUpperTo**(D, V v)</code>: rounds `v[i]`
    towards zero and converts the rounded value to a 64-bit signed or unsigned
    integer, for i in `[Lanes(D()), 2 * Lanes(D()))`. Note that `V` has twice as
    many lanes as `D` and the return value. Returns an implementation-defined
    value if the input exceeds the destination range. Only available if
    `HWY_TARGET != HWY_SCALAR`.

The following may be more convenient or efficient than also calling `ConcatEven`
or `ConcatOdd` followed by `PromoteLowerTo`:

*   `V`:`{u,i}{8,16,32},f{16,32},bf16`, `D`:`RepartitionToWide<DFromV<V>>` \
    <code>Vec&lt;D&gt; **PromoteEvenTo**(D, V v)</code>: promotes the even lanes
    of `v` to `TFromD<D>`. Note that `V` has twice as many lanes as `D` and the
    return value. `PromoteEvenTo(d, v)` is equivalent to, but potentially more
    efficient than `PromoteLowerTo(d, ConcatEven(Repartition<TFromV<V>, D>(), v,
    v))`.

*   `V`:`{u,i}{8,16,32},f{16,32},bf16`, `D`:`RepartitionToWide<DFromV<V>>` \
    <code>Vec&lt;D&gt; **PromoteOddTo**(D, V v)</code>: promotes the odd lanes
    of `v` to `TFromD<D>`. Note that `V` has twice as many lanes as `D` and the
    return value. `PromoteOddTo(d, v)` is equivalent to, but potentially more
    efficient than `PromoteLowerTo(d, ConcatOdd(Repartition<TFromV<V>, D>(), v,
    v))`. Only available if `HWY_TARGET != HWY_SCALAR`.

*   `V`:`f32`, `D`:`{u,i}64` \
    <code>Vec&lt;D&gt; **PromoteInRangeEvenTo**(D, V v)</code>: promotes the
    even lanes of `v` to `TFromD<D>`. Note that `V` has twice as many lanes as
    `D` and the return value. `PromoteInRangeEvenTo(d, v)` is equivalent to, but
    potentially more efficient than `PromoteInRangeLowerTo(d, ConcatEven(
    Repartition<TFromV<V>, D>(), v, v))`.

*   `V`:`f32`, `D`:`{u,i}64` \
    <code>Vec&lt;D&gt; **PromoteInRangeOddTo**(D, V v)</code>: promotes the odd
    lanes of `v` to `TFromD<D>`. Note that `V` has twice as many lanes as `D`
    and the return value. `PromoteInRangeOddTo(d, v)` is equivalent to, but
    potentially more efficient than `PromoteInRangeLowerTo(d, ConcatOdd(
    Repartition<TFromV<V>, D>(), v, v))`.

#### Two-vector demotion

*   `V`,`D`: (`i16,i8`), (`i32,i16`), (`i64,i32`), (`u16,i8`), (`u32,i16`),
    (`u64,i32`), (`i16,u8`), (`i32,u16`), (`i64,u32`), (`u16,u8`), (`u32,u16`),
    (`u64,u32`), (`f32,bf16`) \
    <code>Vec&lt;D&gt; **ReorderDemote2To**(D, V a, V b)</code>: as above, but
    converts two inputs, `D` and the output have twice as many lanes as `V`, and
    the output order is some permutation of the inputs. Only available if
    `HWY_TARGET != HWY_SCALAR`.

*   `V`,`D`: (`i16,i8`), (`i32,i16`), (`i64,i32`), (`u16,i8`), (`u32,i16`),
    (`u64,i32`), (`i16,u8`), (`i32,u16`), (`i64,u32`), (`u16,u8`), (`u32,u16`),
    (`u64,u32`), (`f32,bf16`) \
    <code>Vec&lt;D&gt; **OrderedDemote2To**(D d, V a, V b)</code>: as above, but
    converts two inputs, `D` and the output have twice as many lanes as `V`, and
    the output order is the result of demoting the elements of `a` in the lower
    half of the result followed by the result of demoting the elements of `b` in
    the upper half of the result. `OrderedDemote2To(d, a, b)` is equivalent to
    `Combine(d, DemoteTo(Half<D>(), b), DemoteTo(Half<D>(), a))`, but typically
    more efficient. Note that integer inputs are saturated to the destination
    range as with `DemoteTo`. Only available if `HWY_TARGET != HWY_SCALAR`.

*   `V`,`D`: (`u16,u8`), (`u32,u16`), (`u64,u32`), \
    <code>Vec&lt;D&gt; **OrderedTruncate2To**(D d, V a, V b)</code>: as above,
    but converts two inputs, `D` and the output have twice as many lanes as `V`,
    and the output order is the result of truncating the elements of `a` in the
    lower half of the result followed by the result of truncating the elements
    of `b` in the upper half of the result. `OrderedTruncate2To(d, a, b)` is
    equivalent to `Combine(d, TruncateTo(Half<D>(), b), TruncateTo(Half<D>(),
    a))`, but `OrderedTruncate2To(d, a, b)` is typically more efficient than
    `Combine(d, TruncateTo(Half<D>(), b), TruncateTo(Half<D>(), a))`. Only
    available if `HWY_TARGET != HWY_SCALAR`.

### Combine

*   <code>V2 **LowerHalf**([D, ] V)</code>: returns the lower half of the vector
    `V`. The optional `D` (provided for consistency with `UpperHalf`) is
    `Half<DFromV<V>>`.

All other ops in this section are only available if `HWY_TARGET != HWY_SCALAR`:

*   <code>V2 **UpperHalf**(D, V)</code>: returns upper half of the vector `V`,
    where `D` is `Half<DFromV<V>>`.

*   <code>V **ZeroExtendVector**(D, V2)</code>: returns vector whose `UpperHalf`
    is zero and whose `LowerHalf` is the argument; `D` is `Twice<DFromV<V2>>`.

*   <code>V **Combine**(D, V2, V2)</code>: returns vector whose `UpperHalf` is
    the first argument and whose `LowerHalf` is the second argument; `D` is
    `Twice<DFromV<V2>>`.

**Note**: the following operations cross block boundaries, which is typically
more expensive on AVX2/AVX-512 than per-block operations.

*   <code>V **ConcatLowerLower**(D, V hi, V lo)</code>: returns the
    concatenation of the lower halves of `hi` and `lo` without splitting into
    blocks. `D` is `DFromV<V>`.

*   <code>V **ConcatUpperUpper**(D, V hi, V lo)</code>: returns the
    concatenation of the upper halves of `hi` and `lo` without splitting into
    blocks. `D` is `DFromV<V>`.

*   <code>V **ConcatLowerUpper**(D, V hi, V lo)</code>: returns the inner half
    of the concatenation of `hi` and `lo` without splitting into blocks. Useful
    for swapping the two blocks in 256-bit vectors. `D` is `DFromV<V>`.

*   <code>V **ConcatUpperLower**(D, V hi, V lo)</code>: returns the outer
    quarters of the concatenation of `hi` and `lo` without splitting into
    blocks. Unlike the other variants, this does not incur a block-crossing
    penalty on AVX2/3. `D` is `DFromV<V>`.

*   <code>V **ConcatOdd**(D, V hi, V lo)</code>: returns the concatenation of
    the odd lanes of `hi` and the odd lanes of `lo`.

*   <code>V **ConcatEven**(D, V hi, V lo)</code>: returns the concatenation of
    the even lanes of `hi` and the even lanes of `lo`.

*   <code>V **InterleaveWholeLower**([D, ] V a, V b)</code>: returns
    alternating lanes from the lower halves of `a` and `b` (`a[0]` in the
    least-significant lane). The optional `D` (provided for consistency with
    `InterleaveWholeUpper`) is `DFromV<V>`.

*   <code>V **InterleaveWholeUpper**(D, V a, V b)</code>: returns
    alternating lanes from the upper halves of `a` and `b` (`a[N/2]` in the
    least-significant lane). `D` is `DFromV<V>`.

### Blockwise

**Note**: if vectors are larger than 128 bits, the following operations split
their operands into independently processed 128-bit *blocks*.

*   <code>V **Broadcast**&lt;int i&gt;(V)</code>: returns individual *blocks*,
    each with lanes set to `input_block[i]`, `i = [0, 16/sizeof(T))`.

All other ops in this section are only available if `HWY_TARGET != HWY_SCALAR`:

*   `V`: `{u,i}` \
    <code>VI **TableLookupBytes**(V bytes, VI indices)</code>: returns
    `bytes[indices[i]]`. Uses byte lanes regardless of the actual vector types.
    Results are implementation-defined if `indices[i] < 0` or `indices[i] >=
    HWY_MIN(Lanes(DFromV<V>()), 16)`. `VI` are integers, possibly of a different
    type than those in `V`. The number of lanes in `V` and `VI` may differ, e.g.
    a full-length table vector loaded via `LoadDup128`, plus partial vector `VI`
    of 4-bit indices.

*   `V`: `{u,i}` \
    <code>VI **TableLookupBytesOr0**(V bytes, VI indices)</code>: returns
    `bytes[indices[i]]`, or 0 if `indices[i] & 0x80`. Uses byte lanes regardless
    of the actual vector types. Results are implementation-defined for
    `indices[i] < 0` or in `[HWY_MIN(Lanes(DFromV<V>()), 16), 0x80)`. The
    zeroing behavior has zero cost on x86 and Arm. For vectors of >= 256 bytes
    (can happen on SVE and RVV), this will set all lanes after the first 128
    to 0. `VI` are integers, possibly of a different type than those in `V`. The
    number of lanes in `V` and `VI` may differ.

*   `V`: `{u,i}64`, `VI`: `{u,i}8` \
    <code>V **BitShuffle**(V vals, VI indices)</code>: returns a
    vector with `(vals[i] >> indices[i*8+j]) & 1` in bit `j` of `r[i]` for each
    `j` between 0 and 7.

    `BitShuffle(vals, indices)` zeroes out the upper 56 bits of `r[i]`.

    If `indices[i*8+j]` is less than 0 or greater than 63, bit `j` of `r[i]` is
    implementation-defined.

    `VI` must be either `Vec<Repartition<int8_t, DFromV<V>>>` or
    `Vec<Repartition<uint8_t, DFromV<V>>>`.

    `BitShuffle(v, indices)` is equivalent to the following loop (where `N` is
    equal to `Lanes(DFromV<V>())`):
    ```
    for(size_t i = 0; i < N; i++) {
      uint64_t shuf_result = 0;
      for(int j = 0; j < 7; j++) {
        shuf_result |= ((v[i] >> indices[i*8+j]) & 1) << j;
      }
      r[i] = shuf_result;
    }
    ```

*   <code>V **PairwiseAdd128**(D d, V a, V b)</code>: Add consecutive pairs of
    elements in a and b, and pack results in 128 bit blocks, such that
    `r[i] = a[i] + a[i+1]` for 64 bits, followed by `b[i] + b[i+1]` for next 64
    bits and repeated.

*   <code>V **PairwiseSub128**(D d, V a, V b)</code>: Subtract consecutive pairs
    of elements in a and b, and pack results in 128 bit blocks, such that
    `r[i] = a[i] + a[i+1]` for 64 bits, followed by `b[i] + b[i+1]` for next 64
    bits and repeated.

#### Interleave

Ops in this section are only available if `HWY_TARGET != HWY_SCALAR`:

*   <code>V **InterleaveLower**([D, ] V a, V b)</code>: returns *blocks* with
    alternating lanes from the lower halves of `a` and `b` (`a[0]` in the
    least-significant lane). The optional `D` (provided for consistency with
    `InterleaveUpper`) is `DFromV<V>`.

*   <code>V **InterleaveUpper**(D, V a, V b)</code>: returns *blocks* with
    alternating lanes from the upper halves of `a` and `b` (`a[N/2]` in the
    least-significant lane). `D` is `DFromV<V>`.

*   <code>V **InterleaveEven**([D, ] V a, V b)</code>: returns alternating lanes
    from the even lanes of `a` and `b` (`a[0]` in the least-significant lane,
    followed by `b[0]`, followed by `a[2]`, followed by `b[2]`, and so on). The
    optional `D` (provided for consistency with `InterleaveOdd`) is `DFromV<V>`.
    Note that no lanes move across block boundaries.

    `InterleaveEven(a, b)` and `InterleaveEven(d, a, b)` are both equivalent to
    `OddEven(DupEven(b), a)`, but `InterleaveEven(a, b)` is usually more
    efficient than `OddEven(DupEven(b), a)`.

*   <code>V **InterleaveOdd**(D, V a, V b)</code>: returns alternating lanes
    from the odd lanes of `a` and `b` (`a[1]` in the least-significant lane,
    followed by `b[1]`, followed by `a[3]`, followed by `b[3]`, and so on). `D`
    is `DFromV<V>`. Note that no lanes move across block boundaries.

    `InterleaveOdd(d, a, b)` is equivalent to `OddEven(b, DupOdd(a))`, but
    `InterleaveOdd(d, a, b)` is usually more efficient than `OddEven(b,
    DupOdd(a))`.

#### Zip

*   `Ret`: `MakeWide<T>`; `V`: `{u,i}{8,16,32}` \
    <code>Ret **ZipLower**([DW, ] V a, V b)</code>: returns the same bits as
    `InterleaveLower`, but repartitioned into double-width lanes (required in
    order to use this operation with scalars). The optional `DW` (provided for
    consistency with `ZipUpper`) is `RepartitionToWide<DFromV<V>>`.

*   `Ret`: `MakeWide<T>`; `V`: `{u,i}{8,16,32}` \
    <code>Ret **ZipUpper**(DW, V a, V b)</code>: returns the same bits as
    `InterleaveUpper`, but repartitioned into double-width lanes (required in
    order to use this operation with scalars). `DW` is
    `RepartitionToWide<DFromV<V>>`. Only available if `HWY_TARGET !=
    HWY_SCALAR`.

#### Shift within blocks

Ops in this section are only available if `HWY_TARGET != HWY_SCALAR`:

*   `V`: `{u,i}` \
    <code>V **ShiftLeftBytes**&lt;int&gt;([D, ] V)</code>: returns the result of
    shifting independent *blocks* left by `int` bytes \[1, 15\]. The optional
    `D` (provided for consistency with `ShiftRightBytes`) is `DFromV<V>`.

*   <code>V **ShiftLeftLanes**&lt;int&gt;([D, ] V)</code>: returns the result of
    shifting independent *blocks* left by `int` lanes. The optional `D`
    (provided for consistency with `ShiftRightLanes`) is `DFromV<V>`.

*   `V`: `{u,i}` \
    <code>V **ShiftRightBytes**&lt;int&gt;(D, V)</code>: returns the result of
    shifting independent *blocks* right by `int` bytes \[1, 15\], shifting in
    zeros even for partial vectors. `D` is `DFromV<V>`.

*   <code>V **ShiftRightLanes**&lt;int&gt;(D, V)</code>: returns the result of
    shifting independent *blocks* right by `int` lanes, shifting in zeros even
    for partial vectors. `D` is `DFromV<V>`.

*   `V`: `{u,i}` \
    <code>V **CombineShiftRightBytes**&lt;int&gt;(D, V hi, V lo)</code>: returns
    a vector of *blocks* each the result of shifting two concatenated *blocks*
    `hi[i] || lo[i]` right by `int` bytes \[1, 16). `D` is `DFromV<V>`.

*   <code>V **CombineShiftRightLanes**&lt;int&gt;(D, V hi, V lo)</code>: returns
    a vector of *blocks* each the result of shifting two concatenated *blocks*
    `hi[i] || lo[i]` right by `int` lanes \[1, 16/sizeof(T)). `D` is
    `DFromV<V>`.

#### Other fixed-pattern permutations within blocks

*   <code>V **OddEven**(V a, V b)</code>: returns a vector whose odd lanes are
    taken from `a` and the even lanes from `b`.

*   <code>V **DupEven**(V v)</code>: returns `r`, the result of copying even
    lanes to the next higher-indexed lane. For each even lane index `i`,
    `r[i] == v[i]` and `r[i + 1] == v[i]`.

*   <code>V **DupOdd**(V v)</code>: returns `r`, the result of copying odd lanes
    to the previous lower-indexed lane. For each odd lane index `i`, `r[i] ==
    v[i]` and `r[i - 1] == v[i]`. Only available if `HWY_TARGET != HWY_SCALAR`.

Ops in this section are only available if `HWY_TARGET != HWY_SCALAR`:

*   `V`: `{u,i,f}{32}` \
    <code>V **Shuffle1032**(V)</code>: returns *blocks* with 64-bit halves
    swapped.

*   `V`: `{u,i,f}{32}` \
    <code>V **Shuffle0321**(V)</code>: returns *blocks* rotated right (toward
    the lower end) by 32 bits.

*   `V`: `{u,i,f}{32}` \
    <code>V **Shuffle2103**(V)</code>: returns *blocks* rotated left (toward the
    upper end) by 32 bits.

The following are equivalent to `Reverse2` or `Reverse4`, which should be used
instead because they are more general:

*   `V`: `{u,i,f}{32}` \
    <code>V **Shuffle2301**(V)</code>: returns *blocks* with 32-bit halves
    swapped inside 64-bit halves.

*   `V`: `{u,i,f}{64}` \
    <code>V **Shuffle01**(V)</code>: returns *blocks* with 64-bit halves
    swapped.

*   `V`: `{u,i,f}{32}` \
    <code>V **Shuffle0123**(V)</code>: returns *blocks* with lanes in reverse
    order.

### Swizzle

#### Reverse

*   <code>V **Reverse**(D, V a)</code> returns a vector with lanes in reversed
    order (`out[i] == a[Lanes(D()) - 1 - i]`).

*   <code>V **ReverseBlocks**(V v)</code>: returns a vector with blocks in
    reversed order.

The following `ReverseN` must not be called if `Lanes(D()) < N`:

*   <code>V **Reverse2**(D, V a)</code> returns a vector with each group of 2
    contiguous lanes in reversed order (`out[i] == a[i ^ 1]`).

*   <code>V **Reverse4**(D, V a)</code> returns a vector with each group of 4
    contiguous lanes in reversed order (`out[i] == a[i ^ 3]`).

*   <code>V **Reverse8**(D, V a)</code> returns a vector with each group of 8
    contiguous lanes in reversed order (`out[i] == a[i ^ 7]`).

*   `V`: `{u,i}{16,32,64}` \
    <code>V **ReverseLaneBytes**(V a)</code> returns a vector where the bytes of
    each lane are swapped.

*   `V`: `{u,i}` \
    <code>V **ReverseBits**(V a)</code> returns a vector where the bits of each
    lane are reversed.

#### User-specified permutation across blocks

*   <code>V **TableLookupLanes**(V a, unspecified)</code> returns a vector of
    `a[indices[i]]`, where `unspecified` is the return value of
    `SetTableIndices(D, &indices[0])` or `IndicesFromVec`. The indices are not
    limited to blocks, hence this is slower than `TableLookupBytes*` on
    AVX2/AVX-512. Results are implementation-defined unless `0 <= indices[i] <
    Lanes(D())` and `indices[i] <= LimitsMax<TFromD<RebindToUnsigned<D>>>()`.
    Note that the latter condition is only a (potential) limitation for 8-bit
    lanes on the RVV target; otherwise, `Lanes(D()) <= LimitsMax<..>()`.
    `indices` are always integers, even if `V` is a floating-point type.

*   <code>V **TwoTablesLookupLanes**(D d, V a, V b, unspecified)</code> returns
    a vector of `indices[i] < N ? a[indices[i]] : b[indices[i] - N]`, where
    `unspecified` is the return value of `SetTableIndices(d, &indices[0])` or
    `IndicesFromVec` and `N` is equal to `Lanes(d)`. The indices are not limited
    to blocks. Results are implementation-defined unless `0 <= indices[i] < 2 *
    Lanes(d)` and `indices[i] <= LimitsMax<TFromD<RebindToUnsigned<D>>>()`. Note
    that the latter condition is only a (potential) limitation for 8-bit lanes
    on the RVV target; otherwise, `Lanes(D()) <= LimitsMax<..>()`. `indices` are
    always integers, even if `V` is a floating-point type.

*   <code>V **TwoTablesLookupLanes**(V a, V b, unspecified)</code> returns
    `TwoTablesLookupLanes(DFromV<V>(), a, b, indices)`, see above. Note that the
    results of `TwoTablesLookupLanes(d, a, b, indices)` may differ from
    `TwoTablesLookupLanes(a, b, indices)` on RVV/SVE if `Lanes(d) <
    Lanes(DFromV<V>())`.

*   <code>unspecified **IndicesFromVec**(D d, V idx)</code> prepares for
    `TableLookupLanes` with integer indices in `idx`, which must be the same bit
    width as `TFromD<D>` and in the range `[0, 2 * Lanes(d))`, but need not be
    unique.

*   <code>unspecified **SetTableIndices**(D d, TI* idx)</code> prepares for
    `TableLookupLanes` by loading `Lanes(d)` integer indices from `idx`, which
    must be in the range `[0, 2 * Lanes(d))` but need not be unique. The index
    type `TI` must be an integer of the same size as `TFromD<D>`.

*   <code>V **Per4LaneBlockShuffle**&lt;size_t kIdx3, size_t kIdx2, size_t
    kIdx1, size_t kIdx0&gt;(V v)</code> does a per 4-lane block shuffle of `v`
    if `Lanes(DFromV<V>())` is greater than or equal to 4 or a shuffle of the
    full vector if `Lanes(DFromV<V>())` is less than 4.

    `kIdx0`, `kIdx1`, `kIdx2`, and `kIdx3` must all be between 0 and 3.

    Per4LaneBlockShuffle is equivalent to doing a TableLookupLanes with the
    following indices (but Per4LaneBlockShuffle is more efficient than
    TableLookupLanes on some platforms): `{kIdx0, kIdx1, kIdx2, kIdx3, kIdx0+4,
    kIdx1+4, kIdx2+4, kIdx3+4, ...}`

    If `Lanes(DFromV<V>())` is less than 4 and `kIdx0 >= Lanes(DFromV<V>())` is
    true, Per4LaneBlockShuffle returns an unspecified value in the first lane of
    the result. Otherwise, Per4LaneBlockShuffle returns `v[kIdx0]` in the first
    lane of the result.

    If `Lanes(DFromV<V>())` is equal to 2 and `kIdx1 >= 2` is true,
    Per4LaneBlockShuffle returns an unspecified value in the second lane of the
    result. Otherwise, Per4LaneBlockShuffle returns `v[kIdx1]` in the first lane
    of the result.

#### Slide across blocks

*   <code>V **SlideUpLanes**(D d, V v, size_t N)</code>: slides up `v` by `N`
    lanes

    If `N < Lanes(d)` is true, returns a vector with the first (lowest-index)
    `Lanes(d) - N` lanes of `v` shifted up to the upper (highest-index)
    `Lanes(d) - N` lanes of the result vector and the first (lowest-index) `N`
    lanes of the result vector zeroed out.

    In other words, `result[0..N-1]` would be zero, `result[N] = v[0]`,
    `result[N+1] = v[1]`, and so on until `result[Lanes(d)-1] =
    v[Lanes(d)-1-N]`.

    The result of SlideUpLanes is implementation-defined if `N >= Lanes(d)`.

*   <code>V **SlideDownLanes**(D d, V v, size_t N)</code>: slides down `v` by
    `N` lanes

    If `N < Lanes(d)` is true, returns a vector with the last (highest-index)
    `Lanes(d) - N` of `v` shifted down to the first (lowest-index) `Lanes(d) -
    N` lanes of the result vector and the last (highest-index) `N` lanes of the
    result vector zeroed out.

    In other words, `result[0] = v[N]`, `result[1] = v[N + 1]`, and so on until
    `result[Lanes(d)-1-N] = v[Lanes(d)-1]`, and then `result[Lanes(d)-N..N-1]`
    would be zero.

    The results of SlideDownLanes is implementation-defined if `N >= Lanes(d)`.

*   <code>V **Slide1Up**(D d, V v)</code>: slides up `v` by 1 lane

    If `Lanes(d) == 1` is true, returns `Zero(d)`.

    If `Lanes(d) > 1` is true, `Slide1Up(d, v)` is equivalent to
    `SlideUpLanes(d, v, 1)`, but `Slide1Up(d, v)` is more efficient than
    `SlideUpLanes(d, v, 1)` on some platforms.

*   <code>V **Slide1Down**(D d, V v)</code>: slides down `v` by 1 lane

    If `Lanes(d) == 1` is true, returns `Zero(d)`.

    If `Lanes(d) > 1` is true, `Slide1Down(d, v)` is equivalent to
    `SlideDownLanes(d, v, 1)`, but `Slide1Down(d, v)` is more efficient than
    `SlideDownLanes(d, v, 1)` on some platforms.

*   <code>V **SlideUpBlocks**&lt;int kBlocks&gt;(D d, V v)</code> slides up `v`
    by `kBlocks` blocks.

    `kBlocks` must be between 0 and `d.MaxBlocks() - 1`.

    Equivalent to `SlideUpLanes(d, v, kBlocks * (16 / sizeof(TFromD<D>)))`, but
    `SlideUpBlocks<kBlocks>(d, v)` is more efficient than `SlideUpLanes(d, v,
    kBlocks * (16 / sizeof(TFromD<D>)))` on some platforms.

    The results of `SlideUpBlocks<kBlocks>(d, v)` is implementation-defined if
    `kBlocks >= Blocks(d)` is true.

*   <code>V **SlideDownBlocks**&lt;int kBlocks&gt;(D d, V v)</code> slides down
    `v` by `kBlocks` blocks.

    `kBlocks` must be between 0 and `d.MaxBlocks() - 1`.

    Equivalent to `SlideDownLanes(d, v, kBlocks * (16 / sizeof(TFromD<D>)))`,
    but `SlideDownBlocks<kBlocks>(d, v)` is more efficient than
    `SlideDownLanes(d, v, kBlocks * (16 / sizeof(TFromD<D>)))` on some
    platforms.

    The results of `SlideDownBlocks<kBlocks>(d, v)` is implementation-defined if
    `kBlocks >= Blocks(d)` is true.

#### Other fixed-pattern across blocks

*   <code>V **BroadcastLane**&lt;int kLane&gt;(V v)</code>: returns a vector
    with all of the lanes set to `v[kLane]`.

    `kLane` must be in `[0, MaxLanes(DFromV<V>()))`.

*   <code>V **BroadcastBlock**&lt;int kBlock&gt;(V v)</code>: broadcasts the
    16-byte block of vector `v` at index `kBlock` to all of the blocks of the
    result vector if `Lanes(DFromV<V>()) * sizeof(TFromV<V>) > 16` is true.
    Otherwise, if `Lanes(DFromV<V>()) * sizeof(TFromV<V>) <= 16` is true,
    returns `v`.

    `kBlock` must be in `[0, DFromV<V>().MaxBlocks())`.

*   <code>V **OddEvenBlocks**(V a, V b)</code>: returns a vector whose odd
    blocks are taken from `a` and the even blocks from `b`. Returns `b` if the
    vector has no more than one block (i.e. is 128 bits or scalar).

*   <code>V **SwapAdjacentBlocks**(V v)</code>: returns a vector where blocks of
    index `2*i` and `2*i+1` are swapped. Results are undefined for vectors with
    less than two blocks; callers must first check that via `Lanes`. Only
    available if `HWY_TARGET != HWY_SCALAR`.

*   <code>V **InterleaveEvenBlocks**(D, V a, V b)</code>: returns alternating
    blocks: first/lowest the first from A, then the first from B, then the third
    from A etc. Results are undefined for vectors with less than two blocks;
    callers must first check that via `Lanes`. Only available if `HWY_TARGET !=
    HWY_SCALAR`.

*   <code>V **InterleaveOddBlocks**(D, V a, V b)</code>: returns alternating
    blocks: first/lowest the second from A, then the second from B, then the
    fourth from A etc. Results are undefined for vectors with less than two
    blocks; callers must first check that via `Lanes`. Only available if
    `HWY_TARGET != HWY_SCALAR`.

### Reductions

**Note**: Horizontal operations (across lanes of the same vector) such as
reductions are slower than normal SIMD operations and are typically used outside
critical loops.

The following broadcast the result to all lanes. To obtain a scalar, you can
call `GetLane` on the result, or instead use `Reduce*` below.

*   <code>V **SumOfLanes**(D, V v)</code>: returns the sum of all lanes in each
    lane.
*   <code>V **MinOfLanes**(D, V v)</code>: returns the minimum-valued lane in
    each lane.
*   <code>V **MaxOfLanes**(D, V v)</code>: returns the maximum-valued lane in
    each lane.

The following are equivalent to `GetLane(SumOfLanes(d, v))` etc. but potentially
more efficient on some targets.

*   <code>T **ReduceSum**(D, V v)</code>: returns the sum of all lanes.
*   <code>T **ReduceMin**(D, V v)</code>: returns the minimum of all lanes.
*   <code>T **ReduceMax**(D, V v)</code>: returns the maximum of all lanes.

### Masked reductions

**Note**: Horizontal operations (across lanes of the same vector) such as
reductions are slower than normal SIMD operations and are typically used outside
critical loops.

All ops in this section ignore lanes where `mask=false`. These are equivalent
to, and potentially more efficient than, `GetLane(SumOfLanes(d,
IfThenElseZero(m, v)))` etc. The result is implementation-defined when all mask
elements are false.

*   <code>T **MaskedReduceSum**(D, M m, V v)</code>: returns the sum of all
    lanes where `m[i]` is `true`.
*   <code>T **MaskedReduceMin**(D, M m, V v)</code>: returns the minimum of all
    lanes where `m[i]` is `true`.
*   <code>T **MaskedReduceMax**(D, M m, V v)</code>: returns the maximum of all
    lanes where `m[i]` is `true`.

### Crypto

Ops in this section are only available if `HWY_TARGET != HWY_SCALAR`:

*   `V`: `u8` \
    <code>V **AESRound**(V state, V round_key)</code>: one round of AES
    encryption: `MixColumns(SubBytes(ShiftRows(state))) ^ round_key`. This
    matches x86 AES-NI. The latency is independent of the input values.

*   `V`: `u8` \
    <code>V **AESLastRound**(V state, V round_key)</code>: the last round of AES
    encryption: `SubBytes(ShiftRows(state)) ^ round_key`. This matches x86
    AES-NI. The latency is independent of the input values.

*   `V`: `u8` \
    <code>V **AESRoundInv**(V state, V round_key)</code>: one round of AES
    decryption using the AES Equivalent Inverse Cipher:
    `InvMixColumns(InvShiftRows(InvSubBytes(state))) ^ round_key`. This matches
    x86 AES-NI. The latency is independent of the input values.

*   `V`: `u8` \
    <code>V **AESLastRoundInv**(V state, V round_key)</code>: the last round of
    AES decryption: `InvShiftRows(InvSubBytes(state)) ^ round_key`. This matches
    x86 AES-NI. The latency is independent of the input values.

*   `V`: `u8` \
    <code>V **AESInvMixColumns**(V state)</code>: the InvMixColumns operation of
    the AES decryption algorithm. AESInvMixColumns is used in the key expansion
    step of the AES Equivalent Inverse Cipher algorithm. The latency is
    independent of the input values.

*   `V`: `u8` \
    <code>V **AESKeyGenAssist**&lt;uint8_t kRcon&gt;(V v)</code>: AES key
    generation assist operation

    The AESKeyGenAssist operation is equivalent to doing the following, which
    matches the behavior of the x86 AES-NI AESKEYGENASSIST instruction:
    *  Applying the AES SubBytes operation to each byte of `v`.
    *  Doing a TableLookupBytes operation on each 128-bit block of the
       result of the `SubBytes(v)` operation with the following indices
       (which is broadcast to each 128-bit block in the case of vectors with 32
       or more lanes):
       `{4, 5, 6, 7, 5, 6, 7, 4, 12, 13, 14, 15, 13, 14, 15, 12}`
    *  Doing a bitwise XOR operation with the following vector (where `kRcon`
       is the rounding constant that is the first template argument of the
       AESKeyGenAssist function and where the below vector is broadcasted to
       each 128-bit block in the case of vectors with 32 or more lanes):
       `{0, 0, 0, 0, kRcon, 0, 0, 0, 0, 0, 0, 0, kRcon, 0, 0, 0}`

*   `V`: `u64` \
    <code>V **CLMulLower**(V a, V b)</code>: carryless multiplication of the
    lower 64 bits of each 128-bit block into a 128-bit product. The latency is
    independent of the input values (assuming that is true of normal integer
    multiplication) so this can safely be used in crypto. Applications that wish
    to multiply upper with lower halves can `Shuffle01` one of the operands; on
    x86 that is expected to be latency-neutral.

*   `V`: `u64` \
    <code>V **CLMulUpper**(V a, V b)</code>: as CLMulLower, but multiplies the
    upper 64 bits of each 128-bit block.

## Preprocessor macros

*   `HWY_ALIGN`: Prefix for stack-allocated (i.e. automatic storage duration)
    arrays to ensure they have suitable alignment for Load()/Store(). This is
    specific to `HWY_TARGET` and should only be used inside `HWY_NAMESPACE`.

    Arrays should also only be used for partial (<= 128-bit) vectors, or
    `LoadDup128`, because full vectors may be too large for the stack and should
    be heap-allocated instead (see aligned_allocator.h).

    Example: `HWY_ALIGN float lanes[4];`

*   `HWY_ALIGN_MAX`: as `HWY_ALIGN`, but independent of `HWY_TARGET` and may be
    used outside `HWY_NAMESPACE`.

*   `HWY_RESTRICT`: use after a pointer, e.g. `T* HWY_RESTRICT p`, to indicate
    the pointer is not aliased, i.e. it is the only way to access the data. This
    may improve code generation by preventing unnecessary reloads.

*   `HWY_LIKELY`: use `if (HWY_LIKELY(condition))` to signal to the compiler
    that `condition` is likely to be true. This may improve performance by
    influencing the layout of the generated code.

*   `HWY_UNLIKELY`: like `HWY_LIKELY`, but for conditions likely to be false.

*   `HWY_UNREACHABLE;`: signals to the compiler that control will never reach
    this point, which may improve code generation.

## Advanced macros

Beware that these macros describe the current target being compiled. Imagine a
test (e.g. sort_test) with SIMD code that also uses dynamic dispatch. There we
must test the macros of the target *we will call*, e.g. via `hwy::HaveFloat64()`
instead of `HWY_HAVE_FLOAT64`, which describes the current target.

*   `HWY_IDE` is 0 except when parsed by IDEs; adding it to conditions such as
    `#if HWY_TARGET != HWY_SCALAR || HWY_IDE` avoids code appearing greyed out.

The following indicate full support for certain lane types and expand to 1 or 0.

*   `HWY_HAVE_INTEGER64`: support for 64-bit signed/unsigned integer lanes.
*   `HWY_HAVE_FLOAT16`: support for 16-bit floating-point lanes.
*   `HWY_HAVE_FLOAT64`: support for double-precision floating-point lanes.

The above were previously known as `HWY_CAP_INTEGER64`, `HWY_CAP_FLOAT16`, and
`HWY_CAP_FLOAT64`, respectively. Those `HWY_CAP_*` names are DEPRECATED.

Even if `HWY_HAVE_FLOAT16` is 0, the following ops generally support `float16_t`
and `bfloat16_t`:

*   `Lanes`, `MaxLanes`
*   `Zero`, `Set`, `Undefined`
*   `BitCast`
*   `Load`, `LoadU`, `LoadN`, `LoadNOr`, `LoadInterleaved[234]`, `MaskedLoad`,
    `MaskedLoadOr`
*   `Store`, `StoreU`, `StoreN`, `StoreInterleaved[234]`, `BlendedStore`
*   `PromoteTo`, `DemoteTo`
*   `PromoteUpperTo`, `PromoteLowerTo`
*   `PromoteEvenTo`, `PromoteOddTo`
*   `Combine`, `InsertLane`, `ZeroExtendVector`
*   `RebindMask`, `FirstN`
*   `IfThenElse`, `IfThenElseZero`, `IfThenZeroElse`

Exception: `UpperHalf`, `PromoteUpperTo`, `PromoteOddTo` and `Combine` are not
supported for the `HWY_SCALAR` target.

`Neg` also supports `float16_t` and `*Demote2To` also supports `bfloat16_t`.

*   `HWY_HAVE_SCALABLE` indicates vector sizes are unknown at compile time, and
    determined by the CPU.

*   `HWY_HAVE_CONSTEXPR_LANES` is 1 if `Lanes(d)` is `constexpr`. This is always
    0 `#if HWY_HAVE_SCALABLE`, and may also be 0 in other cases, currently
    including debug builds. `HWY_LANES_CONSTEXPR` replaces the `constexpr`
    keyword for this usage. It expands to `constexpr` or nothing.

*   `HWY_HAVE_TUPLE` indicates `Vec{2-4}`, `Create{2-4}` and `Get{2-4}` are
    usable. This is already true `#if !HWY_HAVE_SCALABLE`, and for SVE targets,
    and the RVV target when using Clang 16. We anticipate it will also become,
    and then remain, true starting with GCC 14.

*   `HWY_MEM_OPS_MIGHT_FAULT` is 1 iff `MaskedLoad` may trigger a (page) fault
    when attempting to load lanes from unmapped memory, even if the
    corresponding mask element is false. This is the case on ASAN/MSAN builds,
    AMD x86 prior to AVX-512, and Arm NEON. If so, users can prevent faults by
    ensuring memory addresses are aligned to the vector size or at least padded
    (allocation size increased by at least `Lanes(d)`).

*   `HWY_NATIVE_FMA` expands to 1 if the `MulAdd` etc. ops use native fused
    multiply-add for floating-point inputs. Otherwise, `MulAdd(f, m, a)` is
    implemented as `Add(Mul(f, m), a)`. Checking this can be useful for
    increasing the tolerance of expected results (around 1E-5 or 1E-6).

*   `HWY_IS_LITTLE_ENDIAN` expands to 1 on little-endian targets and to 0 on
    big-endian targets.

*   `HWY_IS_BIG_ENDIAN` expands to 1 on big-endian targets and to 0 on
    little-endian targets.

The following were used to signal the maximum number of lanes for certain
operations, but this is no longer necessary (nor possible on SVE/RVV), so they
are DEPRECATED:

*   `HWY_CAP_GE256`: the current target supports vectors of >= 256 bits.
*   `HWY_CAP_GE512`: the current target supports vectors of >= 512 bits.

## Detecting supported targets

`SupportedTargets()` returns a non-cached (re-initialized on each call) bitfield
of the targets supported on the current CPU, detected using CPUID on x86 or
equivalent. This may include targets that are not in `HWY_TARGETS`. Conversely,
`HWY_TARGETS` may include unsupported targets. If there is no overlap, the
binary will likely crash. This can only happen if:

*   the specified baseline is not supported by the current CPU, which
    contradicts the definition of baseline, so the configuration is invalid; or
*   the baseline does not include the enabled/attainable target(s), which are
    also not supported by the current CPU, and baseline targets (in particular
    `HWY_SCALAR`) were explicitly disabled.

## Advanced configuration macros

The following macros govern which targets to generate. Unless specified
otherwise, they may be defined per translation unit, e.g. to disable >128 bit
vectors in modules that do not benefit from them (if bandwidth-limited or only
called occasionally). This is safe because `HWY_TARGETS` always includes at
least one baseline target which `HWY_EXPORT` can use.

*   `HWY_DISABLE_CACHE_CONTROL` makes the cache-control functions no-ops.
*   `HWY_DISABLE_BMI2_FMA` prevents emitting BMI/BMI2/FMA instructions. This
    allows using AVX2 in VMs that do not support the other instructions, but
    only if defined for all translation units.

The following `*_TARGETS` are zero or more `HWY_Target` bits and can be defined
as an expression, e.g. `-DHWY_DISABLED_TARGETS=(HWY_SSE4|HWY_AVX3)`.

*   `HWY_BROKEN_TARGETS` defaults to a blocklist of known compiler bugs.
    Defining to 0 disables the blocklist.

*   `HWY_DISABLED_TARGETS` defaults to zero. This allows explicitly disabling
    targets without interfering with the blocklist.

*   `HWY_BASELINE_TARGETS` defaults to the set whose predefined macros are
    defined (i.e. those for which the corresponding flag, e.g. -mavx2, was
    passed to the compiler). If specified, this should be the same for all
    translation units, otherwise the safety check in SupportedTargets (that all
    enabled baseline targets are supported) will not report a mismatch for the
    targets specified in other translation units.

Zero or one of the following macros may be defined to replace the default
policy for selecting `HWY_TARGETS`:

*   `HWY_COMPILE_ONLY_EMU128` selects only `HWY_EMU128`, which avoids intrinsics
    but implements all ops using standard C++.
*   `HWY_COMPILE_ONLY_SCALAR` selects only `HWY_SCALAR`, which implements
    single-lane-only ops using standard C++.
*   `HWY_COMPILE_ONLY_STATIC` selects only `HWY_STATIC_TARGET`, which
    effectively disables dynamic dispatch.
*   `HWY_COMPILE_ALL_ATTAINABLE` selects all attainable targets (i.e. enabled
    and permitted by the compiler, independently of autovectorization), which
    maximizes coverage in tests. Defining `HWY_IS_TEST`, which CMake does for
    the Highway tests, has the same effect.
*   `HWY_SKIP_NON_BEST_BASELINE` compiles all targets at least as good as the
    baseline. This is also the default if nothing is defined. By skipping
    targets older than the baseline, this reduces binary size and may resolve
    compile errors caused by conflicts between dynamic dispatch and -m flags.

At most one `HWY_COMPILE_ONLY_*` may be defined. `HWY_COMPILE_ALL_ATTAINABLE`
may also be defined even if one of `HWY_COMPILE_ONLY_*` is, but will then be
ignored because the flags are tested in the order listed. As an exception,
`HWY_SKIP_NON_BEST_BASELINE` overrides the effect of
`HWY_COMPILE_ALL_ATTAINABLE` and `HWY_IS_TEST`.

## Compiler support

Clang and GCC require opting into SIMD intrinsics, e.g. via `-mavx2` flags.
However, the flag enables AVX2 instructions in the entire translation unit,
which may violate the one-definition rule (that all versions of a function such
as `std::abs` are equivalent, thus the linker may choose any). This can cause
crashes if non-SIMD functions are defined outside of a target-specific
namespace, and the linker happens to choose the AVX2 version, which means it may
be called without verifying AVX2 is indeed supported.

To prevent this problem, we use target-specific attributes introduced via
`#pragma`. Function using SIMD must reside between `HWY_BEFORE_NAMESPACE` and
`HWY_AFTER_NAMESPACE`. Conversely, non-SIMD functions and in particular,
`#include` of normal or standard library headers must NOT reside between
`HWY_BEFORE_NAMESPACE` and `HWY_AFTER_NAMESPACE`. Alternatively, individual
functions may be prefixed with `HWY_ATTR`, which is more verbose, but ensures
that `#include`-d functions are not covered by target-specific attributes.

WARNING: avoid non-local static objects (namespace scope 'global variables')
between `HWY_BEFORE_NAMESPACE` and `HWY_AFTER_NAMESPACE`. We have observed
crashes on PPC because the compiler seems to have generated an initializer using
PPC10 code to splat a constant to all vector lanes, see #1739. To prevent this,
you can replace static constants with a function returning the desired value.

If you know the SVE vector width and are using static dispatch, you can specify
`-march=armv9-a+sve2-aes -msve-vector-bits=128` and Highway will then use
`HWY_SVE2_128` as the baseline. Similarly, `-march=armv8.2-a+sve
-msve-vector-bits=256` enables the `HWY_SVE_256` specialization for Neoverse V1.
Note that these flags are unnecessary when using dynamic dispatch. Highway will
automatically detect and dispatch to the best available target, including
`HWY_SVE2_128` or `HWY_SVE_256`.

Immediates (compile-time constants) are specified as template arguments to avoid
constant-propagation issues with Clang on Arm.

## Type traits

*   `IsFloat<T>()` returns true if the `T` is a floating-point type.
*   `IsSigned<T>()` returns true if the `T` is a signed or floating-point type.
*   `LimitsMin/Max<T>()` return the smallest/largest value representable in
    integer `T`.
*   `SizeTag<N>` is an empty struct, used to select overloaded functions
    appropriate for `N` bytes.

*   `MakeUnsigned<T>` is an alias for an unsigned type of the same size as `T`.

*   `MakeSigned<T>` is an alias for a signed type of the same size as `T`.

*   `MakeFloat<T>` is an alias for a floating-point type of the same size as
    `T`.

*   `MakeWide<T>` is an alias for a type with twice the size of `T` and the same
    category (unsigned/signed/float).

*   `MakeNarrow<T>` is an alias for a type with half the size of `T` and the
    same category (unsigned/signed/float).

## Memory allocation

`AllocateAligned<T>(items)` returns a unique pointer to newly allocated memory
for `items` elements of POD type `T`. The start address is aligned as required
by `Load/Store`. Furthermore, successive allocations are not congruent modulo a
platform-specific alignment. This helps prevent false dependencies or cache
conflicts. The memory allocation is analogous to using `malloc()` and `free()`
with a `std::unique_ptr` since the returned items are *not* initialized or
default constructed and it is released using `FreeAlignedBytes()` without
calling `~T()`.

`MakeUniqueAligned<T>(Args&&... args)` creates a single object in newly
allocated aligned memory as above but constructed passing the `args` argument to
`T`'s constructor and returning a unique pointer to it. This is analogous to
using `std::make_unique` with `new` but for aligned memory since the object is
constructed and later destructed when the unique pointer is deleted. Typically
this type `T` is a struct containing multiple members with `HWY_ALIGN` or
`HWY_ALIGN_MAX`, or arrays whose lengths are known to be a multiple of the
vector size.

`MakeUniqueAlignedArray<T>(size_t items, Args&&... args)` creates an array of
objects in newly allocated aligned memory as above and constructs every element
of the new array using the passed constructor parameters, returning a unique
pointer to the array. Note that only the first element is guaranteed to be
aligned to the vector size; because there is no padding between elements,
the alignment of the remaining elements depends on the size of `T`.

## Speeding up code for older x86 platforms

Thanks to @dzaima for inspiring this section.

It is possible to improve the performance of your code on older x86 CPUs while
remaining portable to all platforms. These older CPUs might indeed be the ones
for which optimization is most impactful, because modern CPUs are usually faster
and thus likelier to meet performance expectations.

For those without AVX3, preferably avoid `Scatter*`; some algorithms can be
reformulated to use `Gather*` instead. For pre-AVX2, it is also important to
avoid `Gather*`.

It is typically much more efficient to pad arrays and use `Load` instead of
`MaskedLoad` and `Store` instead of `BlendedStore`.

If possible, use signed 8..32 bit types instead of unsigned types for
comparisons and `Min`/`Max`.

Other ops which are considerably more expensive especially on SSSE3, and
preferably avoided if possible: `MulEven`, i32 `Mul`, `Shl`/`Shr`,
`Round`/`Trunc`/`Ceil`/`Floor`, float16 `PromoteTo`/`DemoteTo`, `AESRound`.

Ops which are moderately more expensive on older CPUs: 64-bit
`Abs`/`ShiftRight`/`ConvertTo`, i32->u16 `DemoteTo`, u32->f32 `ConvertTo`,
`Not`, `IfThenElse`, `RotateRight`, `OddEven`, `BroadcastSignBit`.

It is likely difficult to avoid all of these ops (about a fifth of the total).
Apps usually also cannot more efficiently achieve the same result as any op
without using it - this is an explicit design goal of Highway. However,
sometimes it is possible to restructure your code to avoid `Not`, e.g. by
hoisting it outside the SIMD code, or fusing with `AndNot` or `CompressNot`.

---

## Release testing process

We run the following before a release:

### Windows x86 host

```
run_tests.bat
```

### Linux x86 host

Clang, GCC; Arm, PPC cross-compile: `./run_tests.sh`

Manual test of WASM and WASM_EMU256 targets.

Check libjxl build actions at https://github.com/libjxl/libjxl/pull/2269. (As of
2025-08-14 this is currently paused and requires a token update)

### Version updates

Prepend to debian/changelog and update mentions of the current version in:

*   base.h
*   CMakeLists.txt
*   MODULE.bazel
*   g3doc/faq.md

### Signing the release

*   `git archive --prefix=highway-X.Y.Z/ -o highway-X.Y.Z.tar.gz X.Y.Z`
*   `gpg --armor --detach-sign highway-X.Y.Z.tar.gz`
*   Edit release and attach the resulting `highway-X.Y.Z.tar.gz.asc` and .gz.

(See https://wiki.debian.org/Creating%20signed%20GitHub%20releases and to obtain
the key, search hkps://keys.openpgp.org for janwas@google.com or since 1.3.0
jan.wassenberg@gmail.com)