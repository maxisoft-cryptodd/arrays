# C++ Zarr-like Library Plan

## 1. Overview

This document outlines the plan to create a high-performance C++ library to replace the existing Python-based `zarr` writing and reading process. The primary goal is to eliminate the Python GIL bottleneck observed during data loading in `okx_ob_dataset.py`.

The library will be a modern C++23 implementation, focusing on performance, concurrency, and extensibility. It will replicate the core functionalities of the current `zarr` setup, including chunking, sharding, and compression, while also allowing for custom data transformations.

## 2. Core Requirements

*   **High Performance:** The library must be significantly faster than the current Python `zarr` implementation, especially for reading data.
*   **Concurrency:** The library must be thread-safe and support concurrent writes and reads.
*   **Compatibility:** The output format should be compatible with the existing `zarr` specification to the extent possible, to allow for a gradual transition.
*   **Extensibility:** The library should be designed to easily incorporate new compression algorithms and custom data transformations (e.g., XOR filters).
*   **Python Integration:** A Python wrapper will be created to allow for a near drop-in replacement of the existing `OrderBookZarrWriter` and `OkxOrderBookZarrReader`.

## 3. Architecture

The library will be composed of several key components:

### 3.1. Storage Engine

This component will be responsible for managing the on-disk storage of the data.

*   **`Store` class:** Represents the root of the data store. It will manage the directory structure and the top-level metadata.
*   **`Array` class:** Represents a single data array within the store. It will manage the array's metadata (shape, dtype, chunks, etc.) and the individual chunks.
*   **`Chunk` class:** Represents a single chunk of data. It will be responsible for compression, decompression, and any custom transformations.

### 3.2. Writer API

The writer API will provide a high-level interface for writing data to the store.

*   **`ZarrWriter` class:** The main entry point for writing data. It will manage a thread pool for concurrent chunk processing and writing.
*   **Asynchronous Operations:** The API will support asynchronous `append` and `write_late_array` operations, mirroring the existing Python implementation.
*   **Metadata Handling:** The `ZarrWriter` will be responsible for collecting and writing all metadata to the store.

### 3.3. Reader API

The reader API will provide a high-level interface for reading data from the store.

*   **`ZarrReader` class:** The main entry point for reading data. It will manage a thread pool for concurrent chunk decompression.
*   **Efficient Data Access:** The API will support efficient slicing and random access of the data.

### 3.4. Compression and Transformation

*   **`Compressor` interface:** A generic interface for compression algorithms. Implementations for `zstd` and `lz4` will be provided.
*   **`Filter` interface:** A generic interface for custom data transformations. An initial `XOR` filter will be implemented as a proof of concept.

### 3.5. Detailed Internals

For a more detailed breakdown of the internal mechanics, including data shapes, chunking/sharding logic, and the two-pass writing process, please refer to the [C++ Library Internals document](./cpp_lib_internals.md).

## 4. Implementation Plan

### Phase 1: Core Library (C++)

1.  **Project Setup:**
    *   Set up a C++23 project using CMake.
    *   Integrate dependencies: `zstd`, `lz4`, `nlohmann/json`, and a testing framework (`GTest` or `Catch2`).
2.  **Storage Engine:**
    *   Implement the `Store`, `Array`, and `Chunk` classes.
    *   Implement the logic for creating and managing the directory structure and metadata files.
3.  **Compression and Transformation:**
    *   Implement the `Compressor` interface and the `zstd` and `lz4` compressors.
    *   Implement the `Filter` interface and a simple `XOR` filter.
4.  **Writer API:**
    *   Implement the `ZarrWriter` class with a thread pool for concurrent writing.
    *   Implement the `append` and `write_late_array` methods.
5.  **Unit Tests:**
    *   Write comprehensive unit tests for all components.

### Phase 2: Python Integration

1.  **Python Wrapper:**
    *   Create a Python wrapper for the C++ library using `pybind11`.
    *   Expose the `ZarrWriter` and `ZarrReader` classes to Python.
2.  **Integration with `cryptodd_predict`:**
    *   Create a new `CppZarrWriter` class in the `cryptodd_predict` project that uses the Python wrapper.
    *   Modify `process_okx_orderbook.py` to use the `CppZarrWriter`.
    *   Create a new `CppZarrReader` class and update `okx_ob_dataset.py` to use it.

### Phase 3: Benchmarking and Optimization

1.  **Benchmarking:**
    *   Create a suite of benchmarks to compare the performance of the C++ library with the original Python `zarr` implementation.
    *   Measure write speed, read speed, and compression ratios.
2.  **Optimization:**
    *   Profile the C++ code to identify and eliminate any performance bottlenecks.
    *   Fine-tune the thread pool and other concurrency parameters.

## 5. Future Enhancements

*   **Additional Compressors:** Add support for other compression algorithms (e.g., `brotli`).
*   **More Filters:** Implement more complex data transformation filters.
*   **Cloud Storage:** Add support for reading and writing to cloud storage backends (e.g., S3).


---


# C++ Zarr-like Library: Internal Design

## 1. Introduction

This document provides a detailed look into the internal design of the C++ library, supplementing the high-level plan in `cpp_lib.md`. It focuses on how the library will replicate and improve upon the specific functionalities of the existing Python-based `zarr` implementation.

## 2. Metadata and On-Disk Structure

The library will adhere to the Zarr v3 specification to ensure compatibility and interoperability.

*   **Directory Structure:** A hierarchical directory structure will be used to represent groups and arrays. For example, the `mid_price` group containing the `raw` array will be stored in a `mid_price/raw/` directory.

*   **Metadata Files:**
    *   `.zgroup`: A JSON file at the root of each group directory, containing `{"zarr_format": 3}` and any group-level attributes.
    *   `.zarray`: A JSON file at the root of each array directory, detailing the array's properties. This is the core of the array's definition.
    *   `.zattrs`: A JSON file containing user-defined attributes for the corresponding group or array. The root `.zattrs` will store the top-level processing metadata.

*   **Chunk Files:** Data chunks will be stored in a `c/` subdirectory within each array's folder. The file naming will follow the chunk grid coordinates (e.g., `c/0`, `c/1` for a 1D array).

## 3. Data Types and Shapes

The C++ library will map NumPy data types to their C++ equivalents. A templated `Array` class will be used to handle different data types.

*   **Type Mapping:**
    *   `float64` -> `double`
    *   `float32` -> `float`
    *   `int64` -> `int64_t`
    *   `uint64` -> `uint64_t`
    *   `bool` -> `uint8_t`

*   **Shape Handling:** The `Array` class will manage a contiguous block of memory (e.g., `std::vector` or `std::unique_ptr<char[]>`) and store the array's shape as a `std::vector<int64_t>`. This allows for handling N-dimensional arrays, although the current use case is primarily 1D and 2D.

## 4. Chunking and Sharding Logic

This is a critical component for performance, and the C++ implementation will closely mirror the logic in `OrderBookZarrWriter`.

### 4.1. Chunk Size Calculation

The goal is to create chunks that are optimized for the filesystem's block size. The `_calculate_chunk_shape` logic will be ported to C++:

1.  Determine the size of a single row of data in bytes.
2.  Estimate the compressed size of a row using the `compression_ratio_hint` from the schema.
3.  Calculate the number of rows (snapshots) that can fit into the target chunk size (e.g., 4 MiB).
4.  Cap the number of snapshots per chunk to a reasonable maximum (e.g., 65,536) to avoid overly large chunks.

### 4.2. Shard Size Calculation

Sharding is used to group multiple chunks into a single file, reducing the total number of files on disk. The logic for determining the shard shape will be:

1.  Calculate the ideal number of snapshots per shard based on a target shard size (e.g., 32 MiB) and the estimated compression ratio.
2.  Determine the total number of shards needed based on the `estimated_total_snapshots`.
3.  Calculate the number of snapshots per shard, ensuring that the final shard dimension is a multiple of the chunk dimension. This prevents a single chunk from spanning multiple shard files.

This entire calculation will be performed once, during the first write operation, to establish the storage layout for each array.

## 5. Writer API and Concurrency

### 5.1. `ZarrWriter` Class

The `ZarrWriter` will be the primary interface for writing data. It will manage a thread pool to parallelize the processing and writing of chunks.

### 5.2. `append` Method

*   **Signature:** `void append(const std::map<std::string, DataChunk>& data_chunks);`
*   **`DataChunk`:** A struct containing a pointer to the raw data, along with its shape and data type.
*   **Workflow:**
    1.  On the first call, the writer will initialize the store: calculate chunk/shard shapes, create the directory hierarchy, and write all `.zgroup` and `.zarray` metadata files.
    2.  For each array in the input map, a task is created and submitted to the thread pool.
    3.  **Write Task:**
        a. Apply any configured filters (e.g., XOR).
        b. Compress the chunk data using the specified codec (e.g., `zstd`).
        c. Write the compressed data to the appropriate chunk file.
    4.  A semaphore will be used to limit the number of concurrent file I/O operations to prevent overwhelming the disk.

### 5.3. `write_late_array` Method

This method is designed for the second pass of the processing pipeline (e.g., writing future opportunities).

*   **Workflow:**
    1.  It will perform the chunking/sharding calculation specifically for this new array.
    2.  It will create the corresponding `.zarray` metadata file.
    3.  It will then proceed to write the data chunks using the same thread pool and task-based approach as the `append` method.
    4.  This operation will be protected by a mutex to ensure that array creation is an atomic operation.

## 6. Reader API and Concurrency

The `ZarrReader` will also use a thread pool to accelerate data loading.

*   **Workflow:**
    1.  When a slice of an array is requested, the reader first parses the `.zarray` metadata to identify the required chunks.
    2.  For each required chunk, a task is submitted to the thread pool.
    3.  **Read Task:**
        a. Read the compressed chunk file from disk.
        b. Decompress the data.
        c. Apply the inverse of any filters.
    4.  The decompressed chunks are then combined into a single contiguous buffer, and the requested slice is copied into the final output array.

## 7. Python Integration with `pybind11`

*   **NumPy Integration:** `pybind11`'s `py::array_t` will be used for zero-copy (where possible) transfer of NumPy arrays between Python and C++.
*   **Writer:** The `ZarrWriter::append` method will be exposed to Python, accepting a dictionary of NumPy arrays.
*   **Reader:** The `ZarrReader::read_slice` method will be exposed, returning a new NumPy array to the Python caller.
*   **Asynchronous Behavior:** The blocking C++ calls will be wrapped in Python using `asyncio.to_thread` to integrate seamlessly with the existing asynchronous Python code.
