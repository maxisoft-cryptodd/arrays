# Overview of Work Done: High-Performance C++ Data Store (`.cdd` Format) - Phase 1

This document provides a comprehensive overview of the work completed in Phase 1: "The Core Library & File Format (No SIMD)" for the high-performance C++ data storage library. The objective is to replace the existing Python-based `zarr` implementation with a custom, performance-first C++ solution, specifically optimized for financial time-series data like OKX order books.

## 1. Guiding Principles and Architectural Shift

The project's direction was fundamentally shaped by prioritizing raw performance and a specialized, custom binary format (`.cdd`) over strict Zarr compatibility. Key principles adopted include:
- **Performance Over Compatibility:** Focus on maximum read/write throughput without constraints from existing formats.
- **Specialization Over Generalization:** Initial design hyper-optimized for the `(N, 50, 3)` OKX order book shape, with generic array handling as a secondary feature.
- **Modern C++ & SIMD:** Leverage C++23 features (`std::float16_t`) and modern SIMD instruction sets (AVX, NEON) via `google/highway` for core data transformations (baseline AVX1).
- **Monolithic File Storage:** Abandon the "many small files" model for a single, append-only binary file per array.
- **Abstraction for Flexibility:** Use abstractions for storage backends (file vs. in-memory) and plan for future multi-language bindings.

## 2. Custom Binary File Format (`.cdd`) Specification

A detailed custom binary file format (`.cdd`) was designed to support efficient storage and retrieval.

### 2.1. Overall Layout

A single `.cdd` file follows this structure, with `CHUNK_OFFSETS` -> `CHUNKS` blocks repeatable for append operations:

```
[File Header]
[Chunk Index #1 (CHUNK_OFFSETS)]
[Data Block #1 (CHUNKS)]
[Chunk Index #2 (CHUNK_OFFSETS)]
[Data Block #2 (CHUNKS)]
...
```

### 2.2. File Header (Written once at creation)

| Field | Type | Description |
| :--- | :--- | :--- |
| `MAGIC` | `uint32` | A constant magic number (`0xDEADBEEF`). |
| `VERSION`| `uint16` | The format version (`1`). |
| `METADATA`| blob | Internal library metadata (Zstd-compressed, length prefixed). |
| `USER_META`| blob | User-provided metadata (Zstd-compressed, length prefixed). |

### 2.3. Chunk Index (`CHUNK_OFFSETS` Block)

This block forms a chain of pre-allocated index blocks, enabling O(1) random access to chunks after initial file parsing and efficient appending.

**Structure:**

| Field | Type | Description |
| :--- | :--- | :--- |
| `size` | `uint32` | Size of this entire block in bytes. |
| `type` | `uint16` | `0`: Raw offsets. `1`: LZ4 compressed array of offsets. |
| `hash` | `uint64[2]`| 128-bit BLAKE3 hash of the `offsets_and_pointer` array. |
| `offsets_and_pointer` | `uint64[]` | An array of `capacity` 64-bit unsigned integers. The last element is the `next_index_offset` pointer (0 if final). |

### 2.4. Data Chunk (`CHUNK`)

This is the container for the actual data.

| Field | Type | Description |
| :--- | :--- | :--- |
| `size` | `uint32` | Size of this entire chunk block in bytes. |
| `type` | `uint16` | `0`: RAW, `1`: ZSTD, `2`: OKX_OB_SIMD_F16, `3`: OKX_OB_SIMD_F32, `4`: GENERIC_OB_SIMD |
| `dtype` | `uint16` | Data type enum (e.g., `float16`, `float32`, `int64`). |
| `hash` | `uint64[2]`| 128-bit BLAKE3 hash of the raw (uncompressed) data. |
| `flags` | `uint64` | Bitmask for transformations (e.g., `LITTLE_ENDIAN`, `DOWN_CAST_16`). |
| `shape` | `uint32[]`| N-dimensional shape, null-terminated (e.g., `[1024, 50, 3, 0]`). |
| `data` | `byte[]` | The payload, processed according to `type` and `flags`. |

## 3. Phase 1 Implementation Details: Core Library & File Format (No SIMD)

This phase focused on establishing the foundational components of the library.

### 3.1. Project Setup
- The project is configured as a C++23 CMake project.
- `vcpkg` is integrated for dependency management, including `zstd`, `lz4`, `blake3`, and `gtest`.
- `CMakeLists.txt` was updated to include new source directories and files as they were created.

### 3.2. Storage Abstraction (`src/storage/storage_backend.h`)
- An interface `cryptodd::IStorageBackend` was defined, specifying methods for `read`, `write`, `seek`, `tell`, `flush`, and `size`.
- Two concrete implementations were provided:
    - `cryptodd::FileBackend`: Handles disk-based I/O using `std::fstream`.
    - `cryptodd::MemoryBackend`: Manages data in an in-memory `std::vector<uint8_t>`.
- A typo (`uint64_t_t` to `uint64_t`) in the `IStorageBackend::write` method signature was corrected.

### 3.3. File Format Structures and Serialization (`src/file_format/cdd_file_format.h`)
- Constants (`CDD_MAGIC`, `CDD_VERSION`) and enums (`ChunkOffsetType`, `ChunkDataType`, `DType`, `ChunkFlags`) were defined to represent the file format's metadata and data characteristics.
- Generic helper functions (`write_pod`, `read_pod`, `write_vector_pod`, `read_vector_pod`, `write_blob`, `read_blob`) were implemented for efficient binary serialization and deserialization of various data types.
- C++ structs `cryptodd::FileHeader`, `cryptodd::ChunkOffsetsBlock`, and `cryptodd::Chunk` were defined, each with `write` and `read` methods to handle their respective binary representations.

### 3.4. Basic Writer API (`src/data_io/data_writer.h`, `src/data_io/data_writer.cpp`)
- The `cryptodd::DataWriter` class was implemented to manage the creation and appending of `.cdd` files.
- **Constructors:** Provided for creating a new file (with optional user metadata and `ChunkOffsetsBlock` capacity) and for opening an existing file for appending.
- **`append_chunk`:** This method takes raw data, its type, dtype, flags, and shape. It handles:
    - Checking if the current `ChunkOffsetsBlock` is full and, if so, writing a new one and updating the pointer in the previous block.
    - Zstd compression of data if `ChunkDataType::ZSTD_COMPRESSED` is specified.
    - BLAKE3 hashing of the *raw* data for integrity.
    - Calculating the chunk's total size and writing the `Chunk` structure to the file.
    - Updating the in-memory `ChunkOffsetsBlock` and rewriting it to disk.
- **Helper Methods:** `compress_zstd` (using `zstd` library) and `calculate_blake3_hash` (using `blake3` library) were implemented.
- **`flush` and `num_chunks`:** Basic methods for flushing buffered writes and reporting the total number of chunks.

### 3.5. Basic Reader API (`src/data_io/data_reader.h`, `src/data_io/data_reader.cpp`)
- The `cryptodd::DataReader` class was implemented to read and access data from `.cdd` files.
- **Constructor:** Opens a `.cdd` file, reads the `FileHeader`, and builds a `master_chunk_offsets_` vector in memory by traversing all `CHUNK_OFFSETS` blocks. This enables O(1) random access to any chunk.
- **`get_chunk`:** Retrieves a specific chunk by its index. It handles:
    - Seeking to the chunk's offset using the `master_chunk_offsets_` index.
    - Reading the `Chunk` structure.
    - Zstd decompression if `ChunkDataType::ZSTD_COMPRESSED` is specified (calculating the expected raw size from shape and dtype).
    - BLAKE3 hash validation of the decompressed data against the stored hash for integrity.
- **`get_chunk_slice`:** Retrieves a range of chunks, returning a vector of raw data buffers.
- **Helper Method:** `decompress_zstd` (using `zstd` library) was implemented.

### 3.6. Unit Tests (`test/main.cpp`)
- Comprehensive unit tests were created using `GTest` to validate the functionality of `FileBackend`, `MemoryBackend`, `DataWriter`, and `DataReader`.
- Tests cover:
    - Writing and reading empty files.
    - Writing and reading single Zstd-compressed chunks with user metadata.
    - Writing and reading multiple chunks within a single `CHUNK_OFFSETS` block.
    - Writing and reading multiple chunks across multiple `CHUNK_OFFSETS` blocks (verifying the chaining mechanism).
    - Appending new chunks to an existing file.
    - Retrieving slices of chunks.
    - Direct functionality tests for `MemoryBackend`.

## 4. Current Status and Next Steps

Phase 1 of the MVP is complete. The core file format, storage abstractions, and basic read/write APIs are implemented and ready for testing. The next logical step is to execute the unit tests to confirm correctness and then proceed to Phase 2, which focuses on SIMD optimization for OKX order book data.
