cmake_minimum_required(VERSION 4.0)

# If on MSVC / Windows, set the vcpkg triplet to x64-windows-static
if(MSVC AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET x64-windows-static CACHE STRING "")
endif()

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

project(cryptodd_arrays VERSION 0.0.1 LANGUAGES CXX)

find_program(CCACHE_PROGRAM ccache)
if(LINUX AND CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
endif()

# ==================================================================================================
# Project Options
# ==================================================================================================
option(USE_MIMALLOC "Enable the mimalloc memory allocator." on)

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
    src/file_format/cdd_file_format.cpp
    src/storage/file_backend.cpp
    src/storage/memory_backend.cpp
    src/data_io/buffer.cpp
    src/storage/mio_backend.cpp
    src/data_io/data_extractor.cpp
    src/data_io/data_compressor.cpp
    src/c_api/cdd_context.cpp
    src/c_api/c_api.cpp
        src/c_api/base64.cpp
        src/c_api/operations/store_chunk_handler.cpp
        src/c_api/operations/store_array_handler.cpp
        src/c_api/operations/inspect_handler.cpp
        src/c_api/operations/load_chunks_handler.cpp
        src/c_api/operations/metadata_handler.cpp
        src/c_api/operations/flush_handler.cpp
        src/c_api/operations/json_serialization.cpp
        src/c_api/operations/store_utils.cpp
        src/c_api/operations/ping_handler.cpp
        src/codecs/float_conversion_simd_codec.cpp
)

set_target_properties(cryptodd_arrays_lib PROPERTIES OUTPUT_NAME "cryptodd_arrays_lib")

# Enable dynamic dispatch for the library target
target_compile_definitions(cryptodd_arrays_lib PRIVATE HWY_EXPORT_AND_DYNAMIC_DISPATCH)

target_include_directories(cryptodd_arrays_lib PUBLIC
    "${CMAKE_BINARY_DIR}"
        "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/src/storage"
    "${CMAKE_SOURCE_DIR}/src/file_format"
    "${CMAKE_SOURCE_DIR}/src/data_io"
    "${CMAKE_SOURCE_DIR}/src/codecs"
    "${CMAKE_SOURCE_DIR}/src/memory"
        "${CMAKE_SOURCE_DIR}/src/c_api"
)

# Link dependencies
find_package(zstd CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)
find_package(blake3 CONFIG REQUIRED)
find_package(stduuid CONFIG REQUIRED)
find_package(hwy CONFIG REQUIRED)
find_package(mio CONFIG REQUIRED)
if (USE_MIMALLOC)
    find_package(mimalloc CONFIG REQUIRED)
endif()
find_package(nlohmann_json CONFIG REQUIRED)
find_package(turbobase64 CONFIG REQUIRED)
find_package(magic_enum CONFIG REQUIRED)

find_package(GTest CONFIG REQUIRED)
find_package(benchmark CONFIG REQUIRED)

find_package(Python COMPONENTS Interpreter Development.Module)
find_package(pybind11 CONFIG REQUIRED)

# Conditionally add and link mimalloc
if(USE_MIMALLOC)
    target_compile_definitions(cryptodd_arrays_lib PRIVATE USE_MIMALLOC)
    # The library links to mimalloc to use its STL allocator internally
    target_link_libraries(cryptodd_arrays_lib PRIVATE mimalloc)
endif()

# Make the library's dependencies public
target_link_libraries(cryptodd_arrays_lib
        PUBLIC
        zstd::libzstd
        stduuid
        nlohmann_json::nlohmann_json
        magic_enum::magic_enum
        PRIVATE
        turbo::base64
        mio::mio mio::mio-headers
        hwy::hwy
        lz4::lz4
        BLAKE3::blake3
)

find_path(MAPBOX_ETERNAL_INCLUDE_DIRS "mapbox/eternal.hpp")
target_include_directories(cryptodd_arrays_lib PRIVATE ${MAPBOX_ETERNAL_INCLUDE_DIRS})

target_compile_definitions(cryptodd_arrays_lib PRIVATE STATIC_LIBRARY_BUILD)

set_target_properties(cryptodd_arrays_lib PROPERTIES POSITION_INDEPENDENT_CODE TRUE)

add_library(cryptodd_arrays_shared SHARED
        library.cpp
        src/c_api/c_api.cpp
)

set_target_properties(cryptodd_arrays_shared PROPERTIES OUTPUT_NAME "cryptodd_arrays")
target_compile_definitions(cryptodd_arrays_shared PRIVATE SHARED_LIBRARY_BUILD)

target_link_libraries(cryptodd_arrays_shared PRIVATE cryptodd_arrays_lib)

# This creates cryptodd_arrays_export.h and defines CRYPTODD_ARRAYS_API
GENERATE_EXPORT_HEADER(cryptodd_arrays_lib)
GENERATE_EXPORT_HEADER(cryptodd_arrays_shared)

target_include_directories(cryptodd_arrays_shared PRIVATE
        "${CMAKE_BINARY_DIR}"
        cryptodd_arrays_lib
)

add_executable(cryptodd_arrays_exe main.cpp)
target_link_libraries(cryptodd_arrays_exe PRIVATE cryptodd_arrays_lib)
if(USE_MIMALLOC)
    target_sources(cryptodd_arrays_exe PRIVATE src/memory/mimalloc_override.cpp)
    target_link_libraries(cryptodd_arrays_exe PRIVATE mimalloc)
    target_compile_definitions(cryptodd_arrays_exe PUBLIC USE_MIMALLOC)
endif()

# Add a test executable
add_executable(cryptodd_arrays_test
        test/lib_simple_tests.cpp
        test/cdd_file_tests.cpp
        test/cdd_compression_tests.cpp
        test/codecs/orderbook_simd_codec_test.cpp
        test/codecs/temporal_1d_simd_codec_test.cpp
        test/codecs/temporal_2d_simd_codec_test.cpp
        test/storage/storage_backend_tests.cpp
        test/test_helpers.cpp
        test/data_io/buffer_test.cpp
        test/c_api/c_api_tests.cpp
        test/helpers/orderbook_generator.cpp
        test/c_api/c_api_orderbook_simd_tests.cpp
        test/codecs/float_conversion_simd_codec_test.cpp
        test/c_api/c_api_temporal_1d_simd_tests.cpp
        test/c_api/c_api_temporal_2d_simd_tests.cpp
)

if(USE_MIMALLOC)
    target_sources(cryptodd_arrays_test PRIVATE src/memory/mimalloc_override.cpp)
    target_link_libraries(cryptodd_arrays_test PRIVATE mimalloc)
    target_compile_definitions(cryptodd_arrays_test PUBLIC USE_MIMALLOC)
endif()

target_link_libraries(cryptodd_arrays_test PRIVATE GTest::gtest_main cryptodd_arrays_lib cryptodd_arrays_shared stduuid)

enable_testing()
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
    target_sources(all_simd_benchmark PRIVATE src/memory/mimalloc_override.cpp)
    target_link_libraries(cryptodd_arrays_test PRIVATE mimalloc)
    target_compile_definitions(all_simd_benchmark PUBLIC USE_MIMALLOC)
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

pybind11_add_module(cryptodd_arrays_py src/python/cryptodd_arrays_pybind11.cpp)

if(LINUX)
    target_compile_features(cryptodd_arrays_py PRIVATE cxx_std_20) # for manylinux_2_28
endif()

target_compile_definitions(cryptodd_arrays_py PRIVATE
        MODULE_NAME="cryptodd_arrays_cpp"
        VERSION_INFO="0.0.1"
)
set_target_properties(cryptodd_arrays_py PROPERTIES OUTPUT_NAME "cryptodd_arrays_cpp")
target_link_libraries(cryptodd_arrays_py PRIVATE pybind11::pybind11)

if(WIN32)
target_sources(cryptodd_arrays_py PRIVATE
        src/python/c_api_loader_win.cpp
)
else()
    target_sources(cryptodd_arrays_py PRIVATE
        src/python/c_api_loader_posix.cpp
)
endif()

target_include_directories(cryptodd_arrays_py PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
)

target_link_directories(cryptodd_arrays_py PRIVATE
        "${CMAKE_BINARY_DIR}"
)

target_compile_definitions(cryptodd_arrays_py PRIVATE PYTHON_LIBRARY_BUILD)

if(DEFINED PY_BUILD_CMAKE_IMPORT_NAME)
    if(MSVC)
        target_compile_options(cryptodd_arrays_lib PRIVATE /MT)
        target_compile_options(cryptodd_arrays_shared PRIVATE /MT)
        target_compile_options(cryptodd_arrays_py PRIVATE /MD)
    endif()
    install(TARGETS cryptodd_arrays_py cryptodd_arrays_shared
        DESTINATION ${PY_BUILD_CMAKE_IMPORT_NAME})
endif()
