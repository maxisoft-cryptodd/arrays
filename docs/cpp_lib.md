# Revised Plan: High-Performance C++ Data Store (`.cdd` Format)

### 1. Guiding Principles (The New Philosophy)

1.  **Performance Over Compatibility:** Our primary goal is maximum read/write throughput. We will not be constrained by any existing format like Zarr.
2.  **Specialization Over Generalization:** The initial design will be hyper-optimized for the `(N, 50, 3)` OKX order book shape. Generic array handling will be a secondary feature built upon this high-performance core.
3.  **Modern C++ & SIMD:** We will leverage C++23 features (`std::float16_t`) and modern SIMD instruction sets (AVX, NEON) for core data transformations. The use of `google/highway` is an excellent suggestion to manage this complexity. The baseline target will be AVX1.
4.  **Monolithic File Storage:** We will abandon the "many small files" model. Data for a given array (e.g., `okx_ob_raw`) will be stored in a single, append-only binary file.
5.  **Abstraction for Flexibility:** While the core is specialized, we will use abstractions for the storage backend (file vs. in-memory) and plan for future multi-language bindings (C#, CLI).

### 2. Custom Binary File Format (`.cdd` - CryptoDD Data)

Based on your design, here is the structured layout for our custom file format.

#### 2.1. Overall Layout

A single `.cdd` file will have the following structure. The `CHUNK_OFFSETS` -> `CHUNKS` blocks can be repeated to support append operations.

```
[File Header]
[Chunk Index #1 (CHUNK_OFFSETS)]
[Data Block #1 (CHUNKS)]
[Chunk Index #2 (CHUNK_OFFSETS)]
[Data Block #2 (CHUNKS)]
...
```

#### 2.2. File Header (Written once at creation)

| Field | Type | Description |
| :--- | :--- | :--- |
| `MAGIC` | `uint32` | A constant magic number (e.g., `0xDEADBEEF`) to identify the file type. |
| `VERSION`| `uint16` | The format version (e.g., `1`). |
| `METADATA`| blob | Internal library metadata (e.g., chunking parameters). `uint32` length prefix, then Zstd-compressed blob. |
| `USER_META`| blob | User-provided metadata. `uint32` length prefix, then Zstd-compressed blob. Library is agnostic to its content. |

#### 2.3. Chunk Index (`CHUNK_OFFSETS` Block)

This block is the core of the file's indexing system. It is a pre-sized array containing file offsets to the actual data chunks. When this block is full, a pointer at the end directs the reader to the next `CHUNK_OFFSETS` block.

**Structure:**

| Field | Type | Description |
| :--- | :--- | :--- |
| `size` | `uint32` | Size of this entire `CHUNK_OFFSETS` block in bytes. |
| `type` | `uint16` | `0`: Raw offsets. `1`: LZ4 compressed array of offsets. |
| `hash` | `uint64[2]`| 128-bit BLAKE3 hash of the `offsets_and_pointer` array for integrity. |
| `offsets_and_pointer` | `uint64[]` | An array of `capacity` 64-bit unsigned integers. |

**Details of `offsets_and_pointer`:**
This is a fixed-size array defined at creation (e.g., 1024 elements).
*   **Elements `0` to `N-1`** are file offsets pointing to the start of each `CHUNK` block.
*   **The very last element (at index `capacity-1`)** is special. It acts as the `next_index_offset` pointer. It contains the file offset to the *next* `CHUNK_OFFSETS` block. A value of `0` indicates this is the final index block in the chain.

#### 2.4. Data Chunk (`CHUNK`)

This is the container for the actual data.

| Field | Type | Description |
| :--- | :--- | :--- |
| `size` | `uint32` | Size of this entire `CHUNK` block in bytes. |
| `type` | `uint16` | `0`: RAW, `1`: ZSTD, `2`: OKX_OB_SIMD_F16, `3`: OKX_OB_SIMD_F32, `4`: GENERIC_OB_SIMD |
| `dtype` | `uint16` | Data type enum (e.g., `float16`, `float32`, `int64`). |
| `hash` | `uint64[2]`| 128-bit BLAKE3 hash of the raw (uncompressed) data for integrity. |
| `flags` | `uint64` | Bitmask for transformations (e.g., `LITTLE_ENDIAN`, `DOWN_CAST_16`). |
| `shape` | `uint32[]`| N-dimensional shape, null-terminated (e.g., `[1024, 50, 3, 0]`). |
| `data` | `byte[]` | The payload, processed according to `type` and `flags`. |

### 3. MVP Implementation Plan: Reader & Writer for OKX Order Book

The goal of the MVP is to prove the performance thesis. We need a writer to create the data and a reader to benchmark it.

**Phase 1: The Core Library & File Format (No SIMD)**

1.  **Project Setup:**
    *   Set up a C++23 CMake project managed with `vcpkg`. (Already done, confirmed by `CMakeLists.txt` and `vcpkg.json`).
    *   Dependencies: `zstd-cpp`, `lz4`, `b3sum` (for BLAKE3), `gtest`. (Already configured in `vcpkg.json` and `CMakeLists.txt`).
2.  **Storage Abstraction:**
    *   Create an `IStorageBackend` interface (`read`, `write`, `seek`, `tell`).
    *   Implement `FileBackend` and `MemoryBackend`.
3.  **File Format Implementation:**
    *   Implement serialization/deserialization for all header and block structures (`File Header`, `CHUNK_OFFSETS`, `CHUNK`). This will involve defining C++ structs or classes that map directly to the binary format.
4.  **Basic Writer API:**
    *   Create a `DataWriter` class.
    *   Implement an `append_chunk` method that takes a raw data buffer and metadata. For now, it will only support the `RAW` and `ZSTD` chunk types. This validates the file structure.
5.  **Basic Reader API:**
    *   Create a `DataReader` class that can open a `.cdd` file, parse all `CHUNK_OFFSETS` blocks, and build a complete in-memory index of all chunks.
    *   Implement `get_chunk(index)` which reads, validates (hash), and decompresses a single chunk.
6.  **Unit Tests:** Create round-trip tests (write then read) to validate correctness for simple Zstd-compressed data.

**Phase 2: The Need for Speed (OKX OB SIMD Optimization)**

1.  **Integrate Google Highway:** Add `hwy` via `vcpkg`. Set up the build system for dynamic dispatch based on available CPU features.
2.  **Implement `OKX_OB_SIMD_F16` Codec:** This is the core task.
    *   Create a C++ function that takes a buffer of shape `(N, 50, 3)` with `float32` data.
    *   Inside, use `hwy` to write optimized loops that perform:
        *   Casting from `float32` to `std::float16_t`.
        *   Applying the multi-startup-point XOR transformation you described.
        *   Compressing the final result with Zstd.
    *   The `DOWN_CAST_16` flag will be set in the chunk header.
3.  **Update Writer/Reader:** Integrate this new codec as `chunk type = 2`. The writer will call this function before writing, and the reader will call its inverse after reading.
4.  **Benchmarking:** Create a dedicated test executable. Generate a large, realistic `(N, 50, 3)` dataset. Benchmark:
    *   Write performance of the new format.
    *   Read performance of a single chunk.
    *   Read performance of a slice `[start:end]`, which is the key metric for the ML data loader.
    *   Compare this against the original Python Zarr implementation reading from its format.

This phased approach ensures we validate the foundational file format first, then focus entirely on the performance-critical SIMD implementation, and finally wrap it for Python use. This directly addresses your priority: **The reader is the MVP, but it needs a writer to produce the data it will read.**
