I have read `src/cryptodd_predict/schemas/targets.py`. This file defines the `FutureOpportunitiesSchema`, which is another concrete implementation of `OutputArraySchema`. It also uses `Shuffle` and `ZstdCodec` for compression, and `VectorizedTemporalCodec` with "xor" transform for filtering. It also includes `calculator_version` and `metrics_enum` in its attributes.

I have now read all the specified files to understand the project's current use of Zarr, including its reading, writing, configuration, and schema definitions. I have a good understanding of the compression (Zstd, Shuffle) and filtering (Delta, AsType, VectorizedTemporalCodec with XOR) mechanisms currently in place.

Here's a summary of what I've learned:
*   **Zarr Reader (`okx_zarr_reader.py`):** Reads data from weekly Zarr stores, handling `data`, `timestamps`, and `snapshot_metadata` arrays. It determines `source_chunk_size` and `estimated_total_snapshots`.
*   **Zarr Writer (`zarr_writer.py`):** Writes data to Zarr stores based on schemas. It calculates optimal chunk and shard shapes, uses `ZstdCodec`, `Shuffle`, `Delta`, `AsType`, and `VectorizedTemporalCodec` (with XOR) for compression and filtering. It supports both initial and "late" array writing.
*   **Configuration (`config.py`):** Defines `zstd_level` and `disable_sharding` as key parameters for Zarr processing.
*   **Base Schemas (`base.py`):** Provides abstract classes for `OutputArraySchema` and `OutputGroupSchema`, defining common properties like `name`, `description`, `columns`, `compression_ratio_hint`, and methods for `get_attributes`, `get_compressors`, and `get_filters`. Default compressor is `ZstdCodec`, and default filter for integers is `Delta`.
*   **Order Book Schemas (`order_book.py`):** Concrete schemas for `NormalizedOBSchema`, `TimestampsSchema`, `MidPriceGroupSchema`, `NormalizationFeaturesSchema`, `SnapshotMetadataSchema`, `PriceVolumeSchema`, and `HurstExponentSchema`. These specify various `compression_ratio_hint` values, use `AsType('float16')` for some arrays, `Shuffle` for multi-dimensional data, `ZstdCodec`, `Delta`, and `VectorizedTemporalCodec(transform="xor", axis=0)`.
*   **Target Schemas (`targets.py`):** `FutureOpportunitiesSchema` also uses `Shuffle`, `ZstdCodec`, and `VectorizedTemporalCodec(transform="xor", axis=0)`.

The custom C++ module will need to replicate the functionality of Zarr, specifically:
1.  **Chunking:** The current system calculates optimal chunk and shard sizes. The C++ module will need a similar mechanism.
2.  **Compression:** Zstd is used extensively. The C++ module should integrate Zstd compression.
3.  **Filtering:**
    *   `Delta` for integer types.
    *   `AsType('float16')` for some float arrays.
    *   `Shuffle` for multi-dimensional arrays.
    *   `VectorizedTemporalCodec(transform="xor", axis=0)` for temporal data. This custom XOR filter is particularly important.
4.  **Storage:** The current system uses a directory of weekly Zarr stores. The C++ module will need to manage file storage in a similar chunked and sharded manner.
5.  **Metadata:** The schemas define attributes for arrays and groups. The C++ module should be able to store and retrieve this metadata.

The user explicitly mentioned "zstd/lz4 compress by chunk, apply custom xor or stuff." This aligns with the current Zarr implementation.

---

I have completed reading all the specified files and have a comprehensive understanding of the current Zarr implementation in the project. This includes how data is read, written, configured, and the specific schemas, compression (Zstd, Shuffle), and filtering (Delta, AsType, VectorizedTemporalCodec with XOR) mechanisms used.

Here is a detailed plan for replacing the `zarr` library with a custom C++ module, which will be documented in `docs/cpp_lib.md` once we switch to ACT MODE:

## C++ 23 Data Storage and Compression Library Plan

### 1. Introduction
The goal is to replace the Python `zarr` library with a custom C++ 23 module to overcome Python GIL limitations and improve performance, especially in data loading for machine learning. This C++ module will handle chunked data storage, compression (Zstd/LZ4), and custom filtering (XOR, Delta, type casting).

### 2. Core Components of the C++ Library

The C++ library will consist of the following main components:

#### 2.1. `ChunkedArrayStore` (or similar name)
This will be the central component responsible for managing the storage of multi-dimensional arrays.
*   **Functionality:**
    *   **Chunking:** Replicate the current chunking logic, including calculating optimal chunk and shard sizes based on data characteristics and target memory/disk usage.
    *   **Storage Layout:** Manage the directory structure for storing chunks, similar to how Zarr organizes its data (e.g., `symbol.zarr/week-WXX.zarr/array_name/chunk_coords`).
    *   **Metadata Management:** Store and retrieve array and group attributes (description, columns, compression hints, etc.) in a structured format (e.g., JSON files within the store).
    *   **Read/Write Operations:** Provide efficient methods for reading and writing chunks, potentially with asynchronous capabilities.

#### 2.2. `Codec` Interface and Implementations
A flexible codec system will be implemented to handle various compression and filtering steps.
*   **`ICodec` Interface:** Define a C++ interface for encoding and decoding data.
*   **Compression Codecs:**
    *   **`ZstdCodec`:** Implement Zstd compression, supporting configurable compression levels (1-22).
    *   **`Lz4Codec`:** Implement LZ4 compression as an alternative, as requested by the user.
*   **Filter Codecs:**
    *   **`DeltaCodec`:** Implement the Delta filter for integer types.
    *   **`AsTypeCodec`:** Implement type casting, specifically `float32` to `float16`, as seen in `NormalizedOBSchema` and `NormalizedMidPriceSchema`.
    *   **`ShuffleCodec`:** Implement the Shuffle filter for byte-level rearrangement, especially for multi-dimensional arrays.
    *   **`VectorizedTemporalXORCodec`:** Implement the custom XOR filter applied along the time axis (axis=0) for temporal data. This is a critical custom component.

#### 2.3. `ArraySchema` and `GroupSchema` Representation
C++ equivalents of the Python `OutputArraySchema` and `OutputGroupSchema` will be needed to define the structure and processing steps for each array.
*   **Schema Definition:** C++ classes or structs to hold array properties (name, description, columns, compression ratio hint, data type, shape, list of codecs).
*   **Schema Serialization/Deserialization:** Ability to save and load schema definitions (e.g., to/from JSON) to ensure consistency between Python and C++ components.

### 3. Integration with Python (Cython)

To allow the existing Python codebase to utilize the new C++ library, a Cython wrapper will be developed.
*   **Cython Bindings:** Create `.pyx` and `.pxd` files to expose the C++ `ChunkedArrayStore` and `Codec` functionalities to Python.
*   **Data Conversion:** Handle efficient conversion of NumPy arrays to C++ data structures and vice-versa, minimizing data copying.

### 4. Migration Strategy

1.  **Phase 1: Develop Core C++ Library:** Implement `ChunkedArrayStore` and the various `Codec` implementations in C++.
2.  **Phase 2: Develop Cython Bindings:** Create the Cython layer to expose the C++ functionality to Python.
3.  **Phase 3: Replace `OkxOrderBookZarrReader`:** Modify `src/cryptodd_predict/io/order_book/okx_zarr_reader.py` to use the new C++ reader via Cython.
4.  **Phase 4: Replace `OrderBookZarrWriter`:** Modify `src/cryptodd_predict/io/order_book/zarr_writer.py` to use the new C++ writer via Cython.
5.  **Phase 5: Update `OkxOrderBookDataset`:** Ensure `src/cryptodd_predict/ml/dataloaders/okx_ob_dataset.py` can seamlessly use the new C++ backend for data loading.
6.  **Phase 6: Performance Validation:** Conduct extensive performance benchmarks to confirm the C++ implementation provides the expected speedup and memory efficiency.

### 5. Key Considerations

*   **Error Handling:** Robust error handling in C++ and proper propagation to Python.
*   **Memory Management:** Careful use of C++ memory management to avoid leaks and ensure efficiency.
*   **Concurrency:** Design the C++ library with concurrency in mind, potentially using C++20 features like coroutines or `std::async` for I/O operations, mirroring the asynchronous nature of the current Python Zarr implementation.
*   **Testing:** Comprehensive unit and integration tests for both the C++ library and its Cython bindings.

This plan provides a clear roadmap for the development of the C++ data storage and compression library.


---

I understand. My apologies for focusing too much on implementation details in the previous plan. I will now provide a more abstract and detailed understanding of the current Zarr implementation, followed by a high-level, abstract plan for the C++ replacement, as requested.

---

## Detailed Understanding of Current Zarr Implementation

The `cryptodd_predict` project leverages the `zarr` library as its primary data storage and retrieval mechanism for order book data. This design choice facilitates efficient, chunked, and compressed storage of large, multi-dimensional numerical arrays, which are typical in financial time-series analysis. The architecture is modular, with distinct components handling I/O, configuration, and data schema definition, all interacting with Zarr.

### 1. Overall Architecture and Data Flow

The data processing pipeline, orchestrated by `scripts/process_okx_orderbook.py`, acts as the factory for the entire system. It instantiates concrete implementations of data readers and writers, which are then injected into the `OrderBookPipeline` (`processing/order_book_pipeline.py`).

*   **`scripts/process_okx_orderbook.py`**: This script is the entry point. It parses command-line arguments, constructs a `OkxOrderBookProcessingConfig` object, and then creates instances of `OkxOrderBookZarrReader` and `OrderBookZarrWriter`. These concrete I/O components, along with a `OkxOrderBookNormalizer`, are passed to the `OrderBookPipeline`.
*   **`OrderBookPipeline`**: This orchestrator operates against abstract interfaces (`IDataReader`, `IDataWriter`). It iteratively fetches raw data chunks from the reader, processes them (e.g., normalization), and then passes the results to the writer. The data contracts flowing through the pipeline are Pydantic models like `OkxOrderBookRawChunk` (from reader) and `OrderBookNormalizationResult` (to writer).

### 2. Zarr Reader (`src/cryptodd_predict/io/order_book/okx_zarr_reader.py`)

The `OkxOrderBookZarrReader` is responsible for abstracting away the complexities of reading raw OKX order book data from Zarr stores.

*   **Purpose**: To provide a stream of raw order book data chunks, organized by symbol and week, to the processing pipeline.
*   **Data Organization**: It expects Zarr stores to be organized into weekly subdirectories (e.g., `symbol.zarr/2023-W01.zarr`). This allows for modular data management and potentially parallel processing of different time periods.
*   **Initialization**: During initialization, it scans all available weekly Zarr stores for a given symbol to:
    *   Estimate the total number of snapshots, which is crucial for pre-allocating resources or providing progress feedback.
    *   Extract metadata (attributes) from the first valid Zarr group, ensuring consistent schema and properties across the dataset.
    *   Determine the `source_chunk_size` (either from `shards[0]` or `chunks[0]`), which informs how data was originally stored and can guide subsequent processing.
*   **Asynchronous I/O**: It utilizes `zarr.api.asynchronous` and `asyncio.Semaphore` to manage concurrent read operations. This is vital for high-throughput data loading, preventing I/O bound bottlenecks by allowing multiple chunks to be fetched in parallel.
*   **Data Yield**: It yields `OkxOrderBookRawChunk` objects, which encapsulate the raw order book data (`raw_ob`), associated timestamps (`timestamps`), and snapshot-level metadata (`snapshot_metadata`).
*   **Robustness**: Includes error handling to skip incomplete or corrupt weekly stores and validates the shape consistency of incoming data chunks, ensuring data integrity throughout the pipeline.

### 3. Zarr Writer (`src/cryptodd_predict/io/order_book/zarr_writer.py`)

The `OrderBookZarrWriter` is responsible for persisting processed order book data into a new Zarr store, adhering to predefined schemas and applying specified compression and filtering.

*   **Purpose**: To write standardized, processed order book data into a structured, compressed, and efficient Zarr format.
*   **Schema-Driven**: It is initialized with a list of `OutputArraySchema` and `OutputGroupSchema` objects. This schema system decouples the writer from the specific data formats, making it highly extensible. The writer flattens these schemas to understand the full hierarchy of arrays and groups to be created.
*   **Optimal Chunking and Sharding**: A key feature is its intelligent calculation of `chunk_shape` and `shard_shape` for each array. This is a performance-critical aspect:
    *   `_TARGET_SHARD_SIZE_BYTES` (32 MiB) and `_MAX_SNAPSHOTS_PER_CHUNK` (65,536) define target sizes.
    *   `compression_ratio_hint` (provided by each schema) is used to estimate compressed data size, allowing the writer to determine how many snapshots can fit into a target chunk/shard size.
    *   `estimated_total_snapshots` (from the reader) helps in calculating the number of shards.
    *   The `disable_sharding` flag allows bypassing sharding, useful for in-memory stores or specific performance profiles.
    *   The `get_optimal_chunk_size` function (from `common/system_info.py`) attempts to determine an optimal chunk size based on the underlying file system, further optimizing I/O.
*   **Hierarchy Creation**: On the first write operation, it asynchronously creates the entire Zarr group and array hierarchy based on the provided schemas and calculated chunk/shard shapes.
*   **Concurrent Appending**: It uses `asyncio.Semaphore` to manage concurrent `append` operations to multiple arrays, maximizing write throughput.
*   **Late Array Writing**: Supports writing arrays that are not available during the main chunk-by-chunk processing loop (e.g., aggregated statistics calculated after all chunks are processed).
*   **Metadata Finalization**: Allows updating root group attributes with final processing metadata upon completion.

### 4. Configuration (`src/cryptodd_predict/core/config.py`)

The `config.py` module defines Pydantic models for various processing configurations, including those directly impacting Zarr operations.

*   **`BaseProcessingConfig`**: Contains general settings like `input_dir`, `output_dir`, `symbols`, `max_concurrent_reads`, `max_concurrent_writes`, `profile_memory`, and crucially, `zstd_level` and `disable_sharding`.
*   **`OkxOrderBookProcessingConfig`**: Inherits from `BaseProcessingConfig` and adds specific settings for the OKX order book pipeline, including `normalizer_settings` and `future_opportunities_settings`.
*   **Impact on Zarr**: The `zstd_level` directly controls the compression strength of the Zstd codec used by the writer, and `disable_sharding` dictates whether the sharding codec is applied.

### 5. Schema System (`src/cryptodd_predict/schemas/base.py`, `src/cryptodd_predict/schemas/order_book.py`, `src/cryptodd_predict/schemas/targets.py`)

The schema system is a cornerstone of the project's modularity and data governance, defining the contract for all data stored in Zarr.

*   **`OutputArraySchema` (Abstract Base Class)**:
    *   **Core Properties**: Defines abstract properties like `name` (array identifier), `description`, and optional `columns` (for multi-dimensional arrays).
    *   **`compression_ratio_hint`**: A critical property that provides an estimated compression ratio (uncompressed size / compressed size). This hint is used by the `OrderBookZarrWriter` to calculate optimal chunk and shard sizes, balancing I/O efficiency with memory usage.
    *   **`get_effective_itemsize`**: Accounts for filters like `AsType` that change the in-memory size of data elements.
    *   **`get_attributes`**: Allows schemas to define custom metadata to be stored with the Zarr array.
    *   **`get_compressors`**: Returns a list of `BytesBytesCodec` objects. By default, it uses `zarr.codecs.ZstdCodec` with the configured `zstd_level`.
    *   **`get_filters`**: Returns a list of `ArrayArrayCodec` objects. By default, it applies `numcodecs.zarr3.Delta` for integer arrays, which is highly effective for time-series data.
*   **`OutputGroupSchema` (Abstract Base Class)**: Defines a logical grouping of related arrays or nested groups, allowing for hierarchical organization within the Zarr store.
*   **Concrete Implementations (`order_book.py`, `targets.py`)**: These files provide specific schemas for different types of data:
    *   **`NormalizedOBSchema`**: Defines the schema for the core normalized order book data. It uses `AsType('float16')` to reduce memory footprint and `VectorizedTemporalCodec(transform="xor", axis=0)` for temporal XOR filtering, indicating a custom, domain-specific optimization. It also uses `Shuffle` before `Zstd`.
    *   **`TimestampsSchema`**: Uses `Delta` and `Zstd` for efficient storage of millisecond timestamps.
    *   **`MidPriceGroupSchema`**: Organizes various mid-price related arrays (raw, normalized, log2-transformed, and their statistics) into a logical group.
    *   **`NormalizationFeaturesSchema`**: Stores log-scaled metadata features, using `Shuffle` and `VectorizedTemporalCodec(transform="xor", axis=0)`.
    *   **`SnapshotMetadataSchema`**: Stores provenance metadata, using `Shuffle` and `VectorizedTemporalCodec(transform="xor", axis=0)`.
    *   **`FutureOpportunitiesSchema`**: Defines the schema for target metrics, also employing `Shuffle` and `VectorizedTemporalCodec(transform="xor", axis=0)`.
*   **Key Codecs and Filters in Use**:
    *   **`ZstdCodec`**: The primary byte-level compressor, used across almost all arrays.
    *   **`Shuffle`**: A byte-level filter often applied before `Zstd` for multi-dimensional arrays, improving compression by grouping similar bytes.
    *   **`Delta`**: An array-to-array filter for integer time-series, storing differences between consecutive values rather than raw values.
    *   **`AsType('float16')`**: An array-to-array filter that casts `float32` data to `float16`, reducing storage and memory requirements at the cost of precision.
    *   **`VectorizedTemporalCodec(transform="xor", axis=0)`**: This is a custom, domain-specific array-to-array filter. It applies an XOR transformation along the time axis (axis=0), which is highly effective for compressing time-series data where consecutive values are often similar. This directly addresses the user's mention of "custom xor or stuff."

### 6. ML Dataloaders (`src/cryptodd_predict/ml/dataloaders/okx_ob_dataset.py`)

The `OkxOrderBookDataset` and its multiprocessing wrapper `MultiProcessOkxDatasetV2` are designed for high-performance data loading for machine learning models.

*   **Purpose**: To efficiently provide sequences of order book data to PyTorch models, minimizing I/O bottlenecks during training.
*   **Asynchronous I/O and Look-Ahead Caching**: The `OkxOrderBookDataset` employs a `ProducerThread` and `CacheManager` to implement a double-buffer system. A background thread asynchronously reads Zarr chunks ahead of time, filling a buffer while the main thread consumes data from another. This virtually eliminates I/O stalls.
*   **Optimized Sampling**: It reads entire Zarr chunks (aligned to `optimal_chunk_size`) and then sub-samples sequences in-memory, maximizing I/O throughput.
*   **Multiprocessing (`MultiProcessOkxDatasetV2`)**: This wrapper uses multiple worker processes (spawned via `mp.get_context("spawn")`) to further parallelize data loading. Each worker runs its own `OkxOrderBookDataset` instance and pushes processed items to a shared `mp.Queue`. This "producer-push" model aims to keep the GPU busy by ensuring a continuous supply of data.
*   **The Bottleneck**: The user explicitly states that "there's a bottle neck in zarr code. hence the rewrite." This bottleneck, even with multiprocessing, is likely due to the Python Global Interpreter Lock (GIL) affecting the underlying `zarr` operations (especially decompression and filtering) within each worker process, limiting true parallelism. The overhead of Python objects and the `zarr` library's Python-level operations, even when backed by C extensions, can still be significant.

---

## Abstract Plan for C++ 23 Data Storage and Compression Library

The objective is to develop a C++ 23 library that provides equivalent or superior functionality to the current Zarr-based system, focusing on high performance, state-of-the-art compression, and custom data transformations. This library will serve as the backend for data storage and retrieval, particularly for the order book processing and ML data loading pipelines.

### 1. Architectural Principles

*   **Performance First**: Designed from the ground up for maximum speed, leveraging C++'s low-level control over memory and execution, and modern concurrency features.
*   **Modularity and Extensibility**: A clear separation of concerns, allowing for easy integration of new compression algorithms, filters, or storage backends.
*   **Schema-Driven**: The storage format and processing steps will be defined by explicit schemas, ensuring data consistency and discoverability.
*   **Chunk-Oriented**: Data will be stored and processed in chunks, optimizing for I/O and memory locality.
*   **Zero-Copy (where possible)**: Minimize data copying between components and between C++ and Python.

### 2. Core Library Components (Abstract View)

#### 2.1. `IChunkedArrayStore` (Concept: Data Storage & Retrieval)

This abstract component will define the interface for interacting with the underlying storage. Concrete implementations will manage the physical layout of data on disk.

*   **Responsibilities**:
    *   **Hierarchical Data Organization**: Manage a hierarchical structure of "groups" and "arrays," similar to a file system or Zarr's group/array model.
    *   **Chunk Management**: Store and retrieve data in discrete, addressable chunks.
    *   **Metadata Persistence**: Store and retrieve arbitrary key-value metadata associated with groups and arrays (e.g., descriptions, column names, processing versions).
    *   **Schema Enforcement**: Ensure that data written conforms to a predefined schema, including data type, shape, and applied codecs.
    *   **Concurrency Control**: Provide mechanisms for safe concurrent access to the store (e.g., for parallel read/write operations).
*   **Key Abstractions**:
    *   `IArray`: Represents a multi-dimensional array within the store, providing methods for reading/writing chunks.
    *   `IGroup`: Represents a hierarchical container for other arrays and groups.
    *   `ChunkAddress`: A mechanism to uniquely identify and locate a specific data chunk within the store.

#### 2.2. `ICodec` (Concept: Data Transformation Pipeline)

This abstract component will define the interface for data transformation (compression and filtering). A chain of `ICodec` implementations will be applied to raw data chunks.

*   **Responsibilities**:
    *   **Encode/Decode**: Transform raw data into a compressed/filtered format and vice-versa.
    *   **Configuration**: Allow codecs to be configured with parameters (e.g., compression level, `astype` target, XOR axis).
    *   **State Management**: Handle any state required for sequential transformations (e.g., `Delta` encoding requires knowledge of the previous value).
*   **Key Abstractions**:
    *   `IBytesBytesCodec`: Transforms a byte stream to another byte stream (e.g., compression).
    *   `IArrayArrayCodec`: Transforms a numerical array to another numerical array (e.g., filtering, type casting).
*   **Concrete Codec Implementations (Examples)**:
    *   **`ZstdCodec`**: High-performance lossless compression.
    *   **`Lz4Codec`**: Extremely fast lossless compression (as requested).
    *   **`DeltaCodec`**: Differential encoding for integer time-series.
    *   **`AsTypeCodec`**: Type casting (e.g., `float32` to `float16`).
    *   **`ShuffleCodec`**: Byte-level permutation for improved compression.
    *   **`TemporalXORCodec`**: Custom XOR transformation along a specified axis for time-series data.

#### 2.3. `ArraySchema` (Concept: Data Contract Definition)

This component will define the blueprint for each array, specifying its characteristics and the sequence of codecs to apply.

*   **Responsibilities**:
    *   **Array Definition**: Hold properties like name, data type, expected shape, and associated metadata.
    *   **Codec Pipeline**: Define the ordered sequence of `ICodec` instances to be applied during encoding and decoding.
    *   **Chunking Strategy**: Specify the preferred chunk dimensions and sharding strategy for the array.
    *   **Serialization**: Allow schemas to be serialized (e.g., to JSON) for persistence and interoperability.

### 3. High-Level Data Flow with C++ Library

1.  **Schema Definition**: Python code defines `ArraySchema` objects, specifying data types, shapes, and the desired `ICodec` pipeline (e.g., `[AsTypeCodec<float16>, TemporalXORCodec, ShuffleCodec, ZstdCodec]`). These schemas are serialized and passed to the C++ layer.
2.  **Writer (C++ `IChunkedArrayStore` implementation)**:
    *   Receives raw data chunks (e.g., from Python via Cython).
    *   Consults the `ArraySchema` to determine the chunking strategy and codec pipeline.
    *   Applies the `ICodec` chain in sequence to encode the data.
    *   Stores the encoded chunks in the physical storage layout.
    *   Persists metadata (including the `ArraySchema`) alongside the data.
3.  **Reader (C++ `IChunkedArrayStore` implementation)**:
    *   Retrieves encoded data chunks from physical storage.
    *   Consults the stored `ArraySchema` to reconstruct the `ICodec` pipeline.
    *   Applies the `ICodec` chain in reverse order to decode the data.
    *   Returns the raw data chunks (e.g., to Python via Cython).

### 4. Integration with Python (Cython Layer)

A thin Cython layer will bridge the Python and C++ worlds.

*   **`cryptodd_predict.cpp_io` (or similar)**: A new Python module (implemented in Cython) that exposes the C++ `IChunkedArrayStore` and `ICodec` functionality.
*   **Data Marshaling**: Efficiently convert NumPy arrays to C++ data structures (e.g., `std::vector<std::byte>`, `xtensor`) and back, minimizing copies.
*   **Schema Translation**: Translate Python Pydantic schema objects into C++ `ArraySchema` objects and vice-versa.

### 5. Migration Strategy (Abstract)

1.  **C++ Core Development**: Implement the `IChunkedArrayStore` interface, a file-system based storage backend, and all required `ICodec` implementations (Zstd, LZ4, Delta, AsType, Shuffle, TemporalXOR).
2.  **Cython Bridge**: Develop the Cython bindings to expose the C++ library to Python.
3.  **Reader Replacement**: Modify the Python `OkxOrderBookZarrReader` to use the new C++ reader via Cython.
4.  **Writer Replacement**: Modify the Python `OrderBookZarrWriter` to use the new C++ writer via Cython.
5.  **ML Dataloader Adaptation**: Update `OkxOrderBookDataset` to leverage the new C++ backend for its chunk reading, ensuring the look-ahead caching and multiprocessing benefits are retained or enhanced.
6.  **Validation & Benchmarking**: Rigorously test for correctness, data integrity, and significant performance improvements (throughput, latency, memory usage) compared to the existing Zarr implementation.

### 6. Key Considerations (Abstract)

*   **Error Semantics**: Define a consistent C++ error handling strategy (e.g., exceptions) and map them appropriately to Python exceptions.
*   **Memory Ownership**: Clearly define memory ownership rules when passing data between C++ and Python to prevent leaks and double-frees.
*   **Concurrency Model**: Design the C++ library's internal concurrency model (e.g., `std::jthread`, `std::async`, custom thread pools) to maximize parallelism without introducing new bottlenecks.
*   **Build System**: Integrate the C++ library into the existing `setup.py` / `pyproject.toml` build system, ensuring seamless compilation of C++ and Cython components.
*   **Backward Compatibility**: Consider strategies for reading existing Zarr stores during a transition period, or a clear migration path for existing data.

This abstract plan provides a comprehensive overview of the architectural considerations and components required for the C++ 23 data storage and compression library. It emphasizes the core concepts and their relationships, setting the stage for detailed design and implementation.

Please let me know if this level of detail and abstraction is satisfactory, and if you are ready to proceed to ACT MODE to create the `docs/cpp_lib.md` file.
</response>
<task_progress>
- [x] Read `okx_zarr_reader.py` to understand Zarr reading
- [x] Read `zarr_writer.py` to understand Zarr writing
- [x] Read `config.py` to understand Zarr configuration
- [x] Read `schemas/base.py` to understand base Zarr schemas
- [x] Read `schemas/order_book.py` to understand order book Zarr schemas
- [x] Read `schemas/targets.py` to understand target Zarr schemas
- [ ] Prepare plan for C++ module in `docs/cpp_lib.md`
  </task_progress>
  </plan_mode_respond>

---


# C++ 23 Data Storage and Compression Library Plan

## 1. Introduction
The goal is to replace the existing Python `zarr` library with a custom C++ 23 module to overcome Python GIL limitations and improve performance, especially in data loading for machine learning. This C++ module will handle chunked data storage, compression (Zstd/LZ4), and custom filtering (XOR, Delta, type casting).

## 2. Core Components of the C++ Library

The C++ library will consist of the following main components:

### 2.1. `IChunkedArrayStore` (Concept: Data Storage & Retrieval)

This abstract component will define the interface for interacting with the underlying storage. Concrete implementations will manage the physical layout of data on disk.

*   **Responsibilities**:
    *   **Hierarchical Data Organization**: Manage a hierarchical structure of "groups" and "arrays," similar to a file system or Zarr's group/array model.
    *   **Chunk Management**: Store and retrieve data in discrete, addressable chunks.
    *   **Metadata Persistence**: Store and retrieve arbitrary key-value metadata associated with groups and arrays (e.g., descriptions, column names, processing versions).
    *   **Schema Enforcement**: Ensure that data written conforms to a predefined schema, including data type, shape, and applied codecs.
    *   **Concurrency Control**: Provide mechanisms for safe concurrent access to the store (e.g., for parallel read/write operations).
*   **Key Abstractions**:
    *   `IArray`: Represents a multi-dimensional array within the store, providing methods for reading/writing chunks.
    *   `IGroup`: Represents a hierarchical container for other arrays and groups.
    *   `ChunkAddress`: A mechanism to uniquely identify and locate a specific data chunk within the store.

### 2.2. `ICodec` (Concept: Data Transformation Pipeline)

This abstract component will define the interface for data transformation (compression and filtering). A chain of `ICodec` implementations will be applied to raw data chunks.

*   **Responsibilities**:
    *   **Encode/Decode**: Transform raw data into a compressed/filtered format and vice-versa.
    *   **Configuration**: Allow codecs to be configured with parameters (e.g., compression level, `astype` target, XOR axis).
    *   **State Management**: Handle any state required for sequential transformations (e.g., `Delta` encoding requires knowledge of the previous value).
*   **Key Abstractions**:
    *   `IBytesBytesCodec`: Transforms a byte stream to another byte stream (e.g., compression).
    *   `IArrayArrayCodec`: Transforms a numerical array to another numerical array (e.g., filtering, type casting).
*   **Concrete Codec Implementations (Examples)**:
    *   **`ZstdCodec`**: High-performance lossless compression.
    *   **`Lz4Codec`**: Extremely fast lossless compression (as requested).
    *   **`DeltaCodec`**: Differential encoding for integer time-series.
    *   **`AsTypeCodec`**: Type casting (e.g., `float32` to `float16`).
    *   **`ShuffleCodec`**: Byte-level permutation for improved compression.
    *   **`TemporalXORCodec`**: Custom XOR transformation along a specified axis for time-series data.

### 2.3. `ArraySchema` (Concept: Data Contract Definition)

This component will define the blueprint for each array, specifying its characteristics and the sequence of codecs to apply.

*   **Responsibilities**:
    *   **Array Definition**: Hold properties like name, data type, expected shape, and associated metadata.
    *   **Codec Pipeline**: Define the ordered sequence of `ICodec` instances to be applied during encoding and decoding.
    *   **Chunking Strategy**: Specify the preferred chunk dimensions and sharding strategy for the array.
    *   **Serialization**: Allow schemas to be serialized (e.g., to JSON) for persistence and interoperability.

### 3. High-Level Data Flow with C++ Library

1.  **Schema Definition**: Python code defines `ArraySchema` objects, specifying data types, shapes, and the desired `ICodec` pipeline (e.g., `[AsTypeCodec<float16>, TemporalXORCodec, ShuffleCodec, ZstdCodec]`). These schemas are serialized and passed to the C++ layer.
2.  **Writer (C++ `IChunkedArrayStore` implementation)**:
    *   Receives raw data chunks (e.g., from Python via Cython).
    *   Consults the `ArraySchema` to determine the chunking strategy and codec pipeline.
    *   Applies the `ICodec` chain in sequence to encode the data.
    *   Stores the encoded chunks in the physical storage layout.
    *   Persists metadata (including the `ArraySchema`) alongside the data.
3.  **Reader (C++ `IChunkedArrayStore` implementation)**:
    *   Retrieves encoded data chunks from physical storage.
    *   Consults the stored `ArraySchema` to reconstruct the `ICodec` pipeline.
    *   Applies the `ICodec` chain in reverse order to decode the data.
    *   Returns the raw data chunks (e.g., to Python via Cython).

### 4. Integration with Python (Cython Layer)

A thin Cython layer will bridge the Python and C++ worlds.

*   **`cryptodd_predict.cpp_io` (or similar)**: A new Python module (implemented in Cython) that exposes the C++ `IChunkedArrayStore` and `ICodec` functionality.
*   **Data Marshaling**: Efficiently convert NumPy arrays to C++ data structures (e.g., `std::vector<std::byte>`, `xtensor`) and vice-versa, minimizing copies.
*   **Schema Translation**: Translate Python Pydantic schema objects into C++ `ArraySchema` objects and vice-versa.

### 5. Migration Strategy (Abstract)

1.  **C++ Core Development**: Implement the `IChunkedArrayStore` interface, a file-system based storage backend, and all required `ICodec` implementations (Zstd, LZ4, Delta, AsType, Shuffle, TemporalXOR).
2.  **Cython Bridge**: Develop the Cython bindings to expose the C++ library to Python.
3.  **Reader Replacement**: Modify the Python `OkxOrderBookZarrReader` to use the new C++ reader via Cython.
4.  **Writer Replacement**: Modify the Python `OrderBookZarrWriter` to use the new C++ writer via Cython.
5.  **ML Dataloader Adaptation**: Update `OkxOrderBookDataset` to leverage the new C++ backend for its chunk reading, ensuring the look-ahead caching and multiprocessing benefits are retained or enhanced.
6.  **Validation & Benchmarking**: Rigorously test for correctness, data integrity, and significant performance improvements (throughput, latency, memory usage) compared to the existing Zarr implementation.

### 6. Key Considerations (Abstract)

*   **Error Semantics**: Define a consistent C++ error handling strategy (e.g., exceptions) and map them appropriately to Python exceptions.
*   **Memory Ownership**: Clearly define memory ownership rules when passing data between C++ and Python to prevent leaks and double-frees.
*   **Concurrency Model**: Design the C++ library's internal concurrency model (e.g., `std::jthread`, `std::async`, custom thread pools) to maximize parallelism without introducing new bottlenecks.
*   **Build System**: Integrate the C++ library into the existing `setup.py` / `pyproject.toml` build system, ensuring seamless compilation of C++ and Cython components.
*   **Backward Compatibility**: Consider strategies for reading existing Zarr stores during a transition period, or a clear migration path for existing data.

This abstract plan provides a comprehensive overview of the architectural considerations and components required for the C++ 23 data storage and compression library. It emphasizes the core concepts and their relationships, setting the stage for detailed design and implementation.


