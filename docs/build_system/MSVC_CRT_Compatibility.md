This document details the architecture and implementation of the CMake build system for `cryptodd-arrays`, focusing specifically on the strategy used to achieve binary portability and compatibility for the Python extension module across Windows and POSIX environments.

***

## Cryptodd-Arrays Build System Documentation: Dynamic Linker Isolation Strategy

### I. Architectural Philosophy

The primary objective of this build system is to create a highly performant, statically linked core C++ library (`cryptodd_arrays_lib`) while distributing a portable Python extension (`cryptodd_arrays_py`).

This architecture employs a **Dynamic Linker Isolation Strategy** to bypass fundamental binary compatibility constraints inherent to the Microsoft Visual C++ (MSVC) toolchain on Windows, particularly concerning the C/C++ Runtime Library (CRT).

### II. Core Components and Linkage Rules

The project is structured around three primary targets, each designed with specific linkage characteristics:

| Target Name | Type | Key Role | Windows Linkage Control |
| :--- | :--- | :--- | :--- |
| **1. `cryptodd_arrays_lib`** | `STATIC` Library | Contains all core C++ logic and performance components (SIMD, file I/O). Links statically to all VCPKG dependencies (Mimalloc, Hwy, Zstd, etc.). | **`/MT` (Multi-threaded Static CRT):** Used to embed all dependency code into the archive. |
| **2. `cryptodd_arrays_shared`** | `SHARED` Library (DLL/SO) | The core application binary interface (ABI) container. This DLL exposes the `extern "C"` API. | **`/MT` (Multi-threaded Static CRT):** Used to ensure the DLL is fully self-contained and manages its own heap/resources isolated from the host process. |
| **3. `cryptodd_arrays_py`** | `pybind11_add_module` (PYD/SO) | The minimal Python wrapper utilizing `pybind11`. | **`/MD` (Multi-threaded Dynamic CRT):** **MANDATORY** for Python ABI compatibility on Windows. |

### III. Rationale: The Dynamic Linker Isolation Strategy

#### A. The Windows Conflict (`LNK2038`)

When building Python extensions on Windows (MSVC), the extension module (`.pyd`) **must** link to the Dynamic C Runtime (`/MD`) to share the process heap and runtime state with the Python interpreter.

If `cryptodd_arrays_shared` (or any static dependency like `mimalloc-static.lib`) were linked directly into `cryptodd_arrays_py`, the linker would detect a critical mismatch: the core logic uses `/MT` (Static CRT) while the host module uses `/MD` (Dynamic CRT). This results in the fatal `LNK2038` error.

#### B. The Brokerage Solution (Runtime Loading)

The `cryptodd-arrays` solution isolates these incompatible environments by decoupling the shared object loading:

1.  **`cryptodd_arrays_shared`** is compiled and installed as a standalone shared library (`cryptodd_arrays.dll` on Windows, `libcryptodd_arrays.so/dylib` on POSIX). It is built entirely with the `/MT` (Static CRT) flag on Windows, ensuring all VCPKG dependencies are bundled and isolated within this file.
2.  **`cryptodd_arrays_py`** is compiled with the required `/MD` (Dynamic CRT) flag and **does not link** to `cryptodd_arrays_shared` at compile time.
3.  The Python module uses platform-specific loader files (`c_api_loader_win.cpp` or `c_api_loader_posix.cpp`) to handle dynamic loading at runtime (when the module is first imported).

| Platform | Loading Mechanism | Implementation |
| :--- | :--- | :--- |
| **Windows (`WIN32`)** | **LoadLibraryA / GetProcAddress** | Loader finds `cryptodd_arrays.dll` (potentially relative to the Python module path) and resolves the `extern "C"` function entry points. |
| **POSIX (`UNIX` or `APPLE`)** | **dlopen / dlsym** | Loader finds `libcryptodd_arrays.so/.dylib` and resolves the `extern "C"` function entry points. |

This strategy ensures memory safety and ABI compliance on Windows by enforcing `/MD` only for the Python-facing object, while `/MT` is used for the complex, dependency-heavy core DLL, maximizing dependency packaging reliability.

### IV. Implementation Details and CMake Configuration

#### A. CRT Flag Enforcement (MSVC Only)

The critical runtime linkage flags are explicitly enforced late in the `CMakeLists.txt` build process, overriding standard defaults:

```cmake
# CMakeLists.txt snippet (MSVC only)
if(DEFINED PY_BUILD_CMAKE_IMPORT_NAME)
    if(MSVC)
        # Core Library targets (STATIC CRT)
        target_compile_options(cryptodd_arrays_lib PRIVATE /MT)
        target_compile_options(cryptodd_arrays_shared PRIVATE /MT) 
        
        # Python Target (DYNAMIC CRT required by Python)
        target_compile_options(cryptodd_arrays_py PRIVATE /MD) 
    endif()
    # ... install targets ...
endif()
```

#### B. Dynamic Loader Inclusion (Cross-Platform)

Source files for the dynamic loader are included conditionally based on the operating system:

```cmake
# CMakeLists.txt snippet (Loader selection)
pybind11_add_module(cryptodd_arrays_py src/python/cryptodd_arrays_pybind11.cpp)
# ... set properties ...

if(WIN32)
    target_sources(cryptodd_arrays_py PRIVATE
        src/python/c_api_loader_win.cpp
    )
else()
    target_sources(cryptodd_arrays_py PRIVATE
        src/python/c_api_loader_posix.cpp
    )
    # Required for dlopen on POSIX systems
    # Note: Explicit linking to 'dl' is often not strictly required by CMake/modern linkers 
    # but is added here for robustness across older/various Unix distributions.
endif()

# Ensure header paths are correct for all targets using the C API
target_include_directories(cryptodd_arrays_py PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
)
```

#### C. Runtime Path Resolution in Loaders

Both `c_api_loader_win.cpp` and `c_api_loader_posix.cpp` implement logic to first attempt loading the core library relative to the imported Python module path (`s_module_path`), ensuring the system finds the DLL/SO even if the directory is not in the system's search path.

#### D. Pybind11 Integration

The `cryptodd_arrays_pybind11.cpp` file initializes the loader (`cryptodd::c_api::setup(module_path)`) upon module import and utilizes the standard C-API functions (which are now provided by the `CApiLoader` wrapper functions) to manage context, execute operations, and translate C++ exceptions into Python exceptions.

### V. Performance Implications

While this strategy introduces an extra layer of indirection (accessing a function via a static pointer lookup rather than a standard IAT lookup), the overhead is **microscopic**. For a system handling high-throughput data operations (like SIMD encoding/decoding), the time spent in the core `cryptodd_arrays_lib` vastly dominates this minor API call overhead. The architectural reliability and portability achieved by this isolation strategy significantly outweigh the negligible performance impact.