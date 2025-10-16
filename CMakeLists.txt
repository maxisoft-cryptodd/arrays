cmake_minimum_required(VERSION 4.0)

# Set up vcpkg integration. This logic must run before the project() command.
# This makes the project easier to configure without relying on presets or command-line args.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    # If CMAKE_TOOLCHAIN_FILE is not set, try to find vcpkg.
    # 1. Check for a VCPKG_ROOT environment variable.
    if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
        set(VCPKG_ROOT_PATH "$ENV{VCPKG_ROOT}")
    # 2. Fallback to a 'vcpkg' subdirectory in the project source.
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(VCPKG_ROOT_PATH "${CMAKE_SOURCE_DIR}/vcpkg")
    endif()

    # If a vcpkg instance was found, set the toolchain file.
    if(DEFINED VCPKG_ROOT_PATH)
        set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT_PATH}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")
        message(STATUS "Vcpkg: CMAKE_TOOLCHAIN_FILE automatically set to: ${CMAKE_TOOLCHAIN_FILE}")
    endif()
endif()

# Suppress the "CMAKE_TOOLCHAIN_FILE not used" warning.
if(DEFINED CMAKE_TOOLCHAIN_FILE)
endif ()

project(cryptodd_arrays)

# ==================================================================================================
# Project Options
# ==================================================================================================
option(USE_MIMALLOC "Enable the mimalloc memory allocator." ON)

# Suppress the "CMAKE_TOOLCHAIN_FILE not used" warning.
if(DEFINED CMAKE_TOOLCHAIN_FILE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Good practice for libraries: hide symbols by default
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

include(GenerateExportHeader)

add_library(cryptodd_arrays_lib STATIC
    src/data_io/data_writer.cpp
    src/file_format/blake3_stream_hasher.cpp
    src/data_io/data_reader.cpp
    src/codecs/zstd_compressor.cpp
    src/codecs/orderbook_simd_codec.cpp
    src/codecs/temporal_1d_simd_codec.cpp
    src/codecs/temporal_2d_simd_codec.cpp
    #src/data_io/data_extractor.cpp
    src/file_format/cdd_file_format.cpp
    src/storage/file_backend.cpp
    src/storage/memory_backend.cpp
    src/data_io/buffer.cpp
    src/storage/mio_backend.cpp
        src/data_io/data_extractor.cpp
        src/data_io/data_compressor.cpp
)

set_target_properties(cryptodd_arrays_lib PROPERTIES OUTPUT_NAME "cryptodd_arrays_lib")

# Enable dynamic dispatch for the library target
target_compile_definitions(cryptodd_arrays_lib PRIVATE HWY_EXPORT_AND_DYNAMIC_DISPATCH)

target_include_directories(cryptodd_arrays_lib PUBLIC
    "${CMAKE_BINARY_DIR}"
    "${CMAKE_SOURCE_DIR}/src/storage"
    "${CMAKE_SOURCE_DIR}/src/file_format"
    "${CMAKE_SOURCE_DIR}/src/data_io"
    "${CMAKE_SOURCE_DIR}/src/codecs"
    "${CMAKE_SOURCE_DIR}/src/memory"
)

# Link dependencies
find_package(zstd CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)
find_package(blake3 CONFIG REQUIRED)
find_package(stduuid CONFIG REQUIRED)
find_package(hwy CONFIG REQUIRED)
find_package(mio CONFIG REQUIRED)
find_package(mimalloc CONFIG REQUIRED)

find_package(GTest CONFIG REQUIRED)
find_package(benchmark CONFIG REQUIRED)

find_package(Python COMPONENTS Interpreter Development.Module)
find_package(pybind11 CONFIG REQUIRED)

# Make the library's dependencies public
target_link_libraries(cryptodd_arrays_lib PUBLIC
        zstd::libzstd
        lz4::lz4
        BLAKE3::blake3
        stduuid
        hwy::hwy
        mio::mio mio::mio-headers
)

# Conditionally add and link mimalloc
if(USE_MIMALLOC)
    target_compile_definitions(cryptodd_arrays_lib PUBLIC USE_MIMALLOC)
    # The library links to mimalloc to use its STL allocator internally
    target_link_libraries(cryptodd_arrays_lib PUBLIC mimalloc-static)
endif()

add_library(cryptodd_arrays_shared SHARED
        library.cpp
)

set_target_properties(cryptodd_arrays_shared PROPERTIES OUTPUT_NAME "cryptodd_arrays")

target_link_libraries(cryptodd_arrays_shared PRIVATE cryptodd_arrays_lib)

# This creates cryptodd_arrays_export.h and defines CRYPTODD_ARRAYS_API
GENERATE_EXPORT_HEADER(cryptodd_arrays_shared)

target_include_directories(cryptodd_arrays_shared PUBLIC
        "${CMAKE_BINARY_DIR}"
        cryptodd_arrays_lib
)

add_executable(cryptodd_arrays_exe main.cpp)
target_link_libraries(cryptodd_arrays_exe PRIVATE cryptodd_arrays_lib)
if(USE_MIMALLOC)
    # For the final executable, we override global new/delete.
    # This is the correct place to do it, not in the library.
    target_sources(cryptodd_arrays_exe PRIVATE src/memory/mimalloc_override.cpp)
endif()

# Add a test executable
add_executable(cryptodd_arrays_test
        test/lib_simple_tests.cpp
        test/cdd_file_tests.cpp
        test/codecs/orderbook_simd_codec_test.cpp
        test/codecs/temporal_1d_simd_codec_test.cpp
        test/codecs/temporal_2d_simd_codec_test.cpp
        test/storage/storage_backend_tests.cpp
        test/test_helpers.cpp
        test/data_io/buffer_test.cpp
)
target_link_libraries(cryptodd_arrays_test PRIVATE GTest::gtest_main cryptodd_arrays_lib cryptodd_arrays_shared stduuid)
if(USE_MIMALLOC)
    target_compile_definitions(cryptodd_arrays_test PUBLIC USE_MIMALLOC)
    target_sources(cryptodd_arrays_test PRIVATE src/memory/mimalloc_override.cpp)
endif()
include(GoogleTest)
gtest_discover_tests(cryptodd_arrays_test)

# Add a benchmark executable
add_executable(all_simd_benchmark
        benchmark/codecs/orderbook_simd_codec_benchmark.cpp
        benchmark/codecs/temporal_1d_simd_codec_benchmark.cpp
        benchmark/codecs/temporal_2d_simd_codec_benchmark.cpp
)

target_link_libraries(all_simd_benchmark PRIVATE
        benchmark::benchmark
        benchmark::benchmark_main
        cryptodd_arrays_lib
)

if(USE_MIMALLOC)
    target_compile_definitions(all_simd_benchmark PUBLIC USE_MIMALLOC)
    # For the bench, we override global new/delete.
    target_sources(all_simd_benchmark PRIVATE src/memory/mimalloc_override.cpp)
endif()

# Add a benchmark executable for the new 1D codec
add_executable(temporal_1d_simd_benchmark
        benchmark/codecs/temporal_1d_simd_codec_benchmark.cpp
)
target_link_libraries(temporal_1d_simd_benchmark PRIVATE
        benchmark::benchmark
        benchmark::benchmark_main
        cryptodd_arrays_lib
)

# Add a benchmark executable for the new 2D codec
add_executable(temporal_2d_simd_benchmark
        benchmark/codecs/temporal_2d_simd_codec_benchmark.cpp
)
target_link_libraries(temporal_2d_simd_benchmark PRIVATE
        benchmark::benchmark
        benchmark::benchmark_main
        cryptodd_arrays_lib
)

pybind11_add_module(cryptodd_arrays_py src/python/cryptodd_arrays_py.cpp)
target_compile_definitions(cryptodd_arrays_py PRIVATE
        MODULE_NAME="cryptodd_arrays_cpp"
        VERSION_INFO="0.0.1"
)
set_target_properties(cryptodd_arrays_py PROPERTIES OUTPUT_NAME "cryptodd_arrays_cpp")
target_link_libraries(cryptodd_arrays_py PRIVATE pybind11::pybind11 cryptodd_arrays_lib)

if(DEFINED PY_BUILD_CMAKE_IMPORT_NAME)
    install(TARGETS cryptodd_arrays_py
        DESTINATION ${PY_BUILD_CMAKE_IMPORT_NAME})
endif()
