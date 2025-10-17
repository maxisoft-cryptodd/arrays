https://aistudio.google.com/app/prompts/141chyppLmOYYv_CiYpKVhfOu0N2xxfyC

Of course. Here is the comprehensive design and implementation plan for the JSON-driven C API. This document synthesizes our entire conversation, detailing the rationale behind each architectural decision and providing a thorough blueprint for development.

***

# Architectural Design Document: The Cryptodd FFI Gateway

**Version:** 1.0
**Status:** Final
**Audience:** Core Developers, Library Integrators, Language Binding Authors

## Table of Contents

1.  **Vision and Guiding Principles**
    1.1. The Mission: A Universal Data Bridge
    1.2. Guiding Architectural Principles

2.  **Core Architecture: The Stateful FFI Gateway**
    2.1. High-Level Diagram
    2.2. The Stateful Context Model
    2.3. Rationale: Why Stateful is Essential
    2.4. Component Responsibilities

3.  **The C Foreign Function Interface (FFI) Layer**
    3.1. API Philosophy: Simplicity, Portability, Stability
    3.2. Header Definition (`cryptodd_c_api.h`)
    3.3. Function Signatures and Contracts
    3.3.1. `cdd_context_create`
    3.3.2. `cdd_context_destroy`
    3.3.3. `cdd_execute_op`
    3.3.4. `cdd_error_message`
    3.4. Error Handling and Status Codes
    3.5. Data Types and Conventions

4.  **The JSON-RPC Protocol**
    4.1. General Schema Principles
    4.1.1. Versioning
    4.1.2. Request and Response Structure
    4.2. Context Creation (`cdd_context_create`) JSON Schema
    4.3. Operation Execution (`cdd_execute_op`) JSON Schemas
    4.3.1. **Store Operations**
    4.3.1.1. `op_type: "StoreChunk"`
    4.3.1.2. `op_type: "StoreArray"`
    4.3.1.3. Common `encoding` and `data_spec` Blocks
    4.3.2. **Load Operations**
    4.3.2.1. `op_type: "LoadChunks"`
    4.3.2.2. The `selection` and `validation` Blocks
    4.3.3. **Metadata and Inspection Operations**
    4.3.3.1. `op_type: "Inspect"`
    4.3.3.2. `op_type: "GetUserMetadata"`
    4.3.3.3. `op_type: "SetUserMetadata"`
    4.3.4. **Control Operations**
    4.3.4.1. `op_type: "Flush"`
    4.4. The JSON Response Schema
    4.4.1. Success Response Structure
    4.4.2. Error Response Structure

5.  **C++ Core Implementation Strategy**
    5.1. The `CddContext` Class: The Heart of the Session
    5.2. Global Handle Management
    5.3. FFI Function Implementations
    5.4. The Operation Dispatcher and Handler Pattern
    5.5. State Management for Temporal Codecs
    5.6. Workspace and Resource Management
    5.7. Error Propagation: `std::expected` to FFI Error Codes

6.  **Language Binding Strategies**
    6.1. Python (pybind11) Binding
    6.1.1. The Wrapper Class (`cdd.File`)
    6.1.2. Resource Management with `__enter__`/`__exit__`
    6.1.3. Implementing `store()` and `load()`
    6.1.4. Buffer Protocol and Data Marshalling
    6.2. C# (P/Invoke) Binding
    6.2.1. The Wrapper Class (`CddFile` implementing `IDisposable`)
    6.2.2. Safe Handles for Context Management
    6.2.3. Data Marshalling with `Marshal`
    6.3. Direct C/C++ Usage

7.  **In-Depth Design Rationale & Q&A**
    7.1. Q1: Why a stateful FFI (context handle) instead of a simpler stateless one?
    7.2. Q2: Why JSON instead of a binary format like Protobuf or FlatBuffers?
    7.3. Q3: Why was the zero-copy "view" for Python rejected in favor of a copy-on-load?
    7.4. Q4: How is thread safety handled?
    7.5. Q5: How will the system handle endianness?
    7.6. Q6: How is the API designed for future extensibility?

8.  **Implementation Plan**
    8.1. Phase 1: Core FFI and Context Management
    8.2. Phase 2: Store Operations
    8.3. Phase 3: Load and Inspect Operations
    8.4. Phase 4: Python and C# Bindings
    8.5. Phase 5: Documentation and Examples

---

## 1. Vision and Guiding Principles

### 1.1. The Mission: A Universal Data Bridge
The primary mission of this high-level API is to serve as a robust, high-performance, and language-agnostic bridge between application memory and the specialized `.cdd` compressed storage format. It must abstract away the complexities of chunking, SIMD-accelerated codecs, and file I/O, presenting a clean, logical interface for storing and retrieving multi-dimensional array data. This API is the gateway through which all external systems will interact with the core library.

### 1.2. Guiding Architectural Principles

Every design decision in this document is guided by the following principles, in order of priority:

1.  **Correctness and Robustness:** The API must be safe, predictable, and free of undefined behavior. It must handle errors gracefully and provide clear, actionable feedback to the caller. Data integrity is paramount.
2.  **Stability:** The C FFI layer constitutes a public contract. Its signature and fundamental behavior must be exceptionally stable to avoid breaking downstream language bindings with each new library version.
3.  **Clarity and Usability:** The API, particularly when wrapped in a language-native binding, should be intuitive and idiomatic. The complexity of the core library should be hidden, not exposed.
4.  **Performance:** While abstracting complexity, the API must not introduce undue overhead. It should enable near-native performance for I/O and computation, leveraging the full power of the underlying SIMD codecs.
5.  **Extensibility:** The design must anticipate future needs. Adding new codecs, storage backends, or operations should be possible without redesigning the entire API.

## 2. Core Architecture: The Stateful FFI Gateway

The chosen architecture is a **Stateful FFI Gateway**. This model provides a single, strictly-defined C interface that manages persistent sessions (or "contexts") for interacting with storage resources.

### 2.1. High-Level Diagram

```
┌───────────────────┐      ┌───────────────────┐      ┌───────────────────┐
│ Python Binding    │      │   C# Binding      │      │    Other FFI      │
│ (pybind11)        │      │   (P/Invoke)      │      │    Clients (C/++) │
└───────────────────┘      └───────────────────┘      └───────────────────┘
         │                        │                        │
         └───────────────┬────────┴─────────────────┬──────┘
                         │   (JSON + Raw Pointers)  │
                         ▼                          ▼
+--------------------------------------------------------------------------+
|                       FFI GATEWAY (cryptodd_c_api.h)                     |
|--------------------------------------------------------------------------|
|   int64_t cdd_context_create(...)  -> returns handle                     |
|   int64_t cdd_execute_op(handle, ...) -> performs actions                |
|   int64_t cdd_context_destroy(handle) -> cleans up                       |
+--------------------------------------------------------------------------+
                         │
                         ▼ (Internal C++ API)
+--------------------------------------------------------------------------+
|                       C++ Core Implementation                            |
|--------------------------------------------------------------------------|
| [Global Handle -> CddContext Map] (Thread-Safe)                          |
|                                                                          |
|     ┌────────────────────────┐      ┌────────────────────────────────┐   |
|     │ CddContext Instance    │      │ Operation Dispatcher           │   |
|     │ --------------------   │      │ ----------------------------   │   |
|     │ - DataReader/Writer    │◄─────┤ - Parse/Validate JSON Request  │   |
|     │ - Codec Workspaces     │      │ - Select Operation Handler     │   |
|     │ - Temporal State       │      └────────────────────────────────┘   |
|     └────────────────────────┘                  │                        |
|                                                 ▼                        |
|                               ┌───────────────────────────────────┐      |
|                               │ IOperationHandler Implementations │      |
|                               │ (Store, Load, Inspect...)         │      |
|                               └───────────────────────────────────┘      |
+--------------------------------------------------------------------------+
```

### 2.2. The Stateful Context Model
The core concept is the `CddContext`. A context is an opaque object, managed by the C++ core, that represents an active session with a single data resource (one file or one in-memory dataset).

-   A client **creates** a context via `cdd_context_create`, specifying the backend and mode. They receive an opaque `intptr_t` handle in return.
-   The client then **executes** one or more operations on this handle via `cdd_execute_op`. The context maintains all necessary state between these calls (e.g., file pointers, `DataWriter` state, temporal codec history).
-   Finally, the client **destroys** the context via `cdd_context_destroy`, which guarantees that all resources are flushed and released.

### 2.3. Rationale: Why Stateful is Essential
A stateless model (where every function call is self-contained) was considered and rejected. It is fundamentally unsuitable for this library's requirements due to:
-   **Gross Inefficiency:** A stateless model would require re-opening files and re-parsing headers for every chunk-level operation.
-   **Stateful by Nature:** The `DataWriter` is inherently stateful; it must track the current chunk index, offsets block, and file position to correctly append new data. This state cannot be managed across independent, stateless function calls.
-   **Resource Management:** Critical resources like SIMD codec workspaces must be reused across operations on the same dataset to avoid constant, expensive reallocations. A stateful context is the natural owner of these resources.

### 2.4. Component Responsibilities
-   **Language Bindings:** Provide an idiomatic, high-level wrapper. Responsible for managing the context lifecycle (e.g., in a Python `with` block) and translating native data types (e.g., NumPy arrays) into pointers and JSON requests.
-   **FFI Gateway:** The stable C ABI. Defines the public contract. It is a thin, exception-safe layer that translates C types into C++ types and calls the core.
-   **C++ Core:** The engine. Manages a global, thread-safe map of handles to `CddContext` instances. Implements the operation dispatcher and the logic for all data manipulation.

## 3. The C Foreign Function Interface (FFI) Layer

This is the public contract of the library. It is designed for maximum portability and stability.

### 3.1. API Philosophy: Simplicity, Portability, Stability
-   **C ABI:** Uses only standard C types (`const char*`, `size_t`, `int64_t`, `intptr_t`, `void*`). No `typedef`s for primitive types are exposed to ensure maximum compatibility.
-   **Error Reporting:** Functions return a simple `int64_t` status code. `0` indicates success, negative values indicate errors. A helper function is provided to get a descriptive string for an error code. Detailed, structured error information is provided in the JSON response.
-   **Opaque Handles:** All state is managed behind an opaque `intptr_t` handle. The caller never interacts with internal C++ objects.

### 3.2. Header Definition (`cryptodd_c_api.h`)
```c
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to a CddContext instance.
// Using intptr_t for maximum portability to store a pointer.
#if !defined(intptr_t)
# if defined(_WIN64) || defined(__x86_64__)
typedef int64_t intptr_t;
# else
typedef int32_t intptr_t;
# endif
#endif

/**
 * @brief Creates a context for interacting with a .cdd resource.
 *
 * @param json_config A UTF-8 encoded JSON string configuring the backend.
 * @param config_len Length of the JSON string in bytes.
 * @param out_handle A pointer to receive the opaque context handle on success.
 * @return int64_t 0 on success, negative error code on failure.
 */
int64_t cdd_context_create(
    const char* json_config,
    size_t config_len,
    intptr_t* out_handle
);

/**
 * @brief Destroys a context and releases all associated resources.
 *
 * @param handle The context handle to destroy.
 * @return int64_t 0 on success, negative error code on failure.
 */
int64_t cdd_context_destroy(intptr_t handle);

/**
 * @brief Executes an operation on a given context.
 *
 * @param handle The context handle.
 * @param json_op_request A UTF-8 encoded JSON string describing the operation.
 * @param request_len Length of the JSON request string.
 * @param input_data_ptr Pointer to the source data buffer (used by 'Store' operations).
 * @param input_data_bytes Size of the input data buffer in bytes.
 * @param output_data_ptr Pointer to the destination buffer (used by 'Load' operations).
 * @param max_output_data_bytes Capacity of the output buffer in bytes.
 * @param json_op_response Buffer to write the UTF-8 encoded JSON response into. Must be large enough.
 * @param max_json_response_bytes Capacity of the JSON response buffer.
 * @return int64_t 0 on success, negative error code on failure. The details of the success or
 *         failure are written into the `json_op_response` buffer.
 */
int64_t cdd_execute_op(
    intptr_t handle,
    const char* json_op_request,
    size_t request_len,
    const void* input_data_ptr,
    int64_t input_data_bytes,
    void* output_data_ptr,
    int64_t max_output_data_bytes,
    char* json_op_response,
    size_t max_json_response_bytes
);

/**
 * @brief Translates an error code from the API into a human-readable string.
 *
 * @param error_code The negative error code returned by an API function.
 * @return const char* A static, null-terminated string describing the error. Returns "Unknown Error" for invalid codes.
 */
const char* cdd_error_message(int64_t error_code);

#ifdef __cplusplus
}
#endif
```

### 3.3. Function Signatures and Contracts

#### 3.3.1. `cdd_context_create`
-   **Purpose:** The entry point. Initializes a session with a backend.
-   **`json_config`:** A JSON string detailing the backend. See Section 4.2.
-   **`out_handle`:** On success, this will be populated with a non-zero handle.
-   **Return Value:** `0` on success. A negative `int64_t` error code on failure (e.g., invalid JSON, file not found, permission denied).
-   **Implementation Note:** This function allocates a `CddContext` C++ object and stores it in a global, thread-safe map, returning an integer handle that acts as the key.

#### 3.3.2. `cdd_context_destroy`
-   **Purpose:** The exit point. Guarantees resource cleanup.
-   **`handle`:** The handle obtained from `cdd_context_create`.
-   **Return Value:** `0` on success. A negative code if the handle is invalid.
-   **Implementation Note:** This function looks up the handle in the global map, destroys the associated `CddContext` object (which will flush files in its destructor), and removes the entry from the map.

#### 3.3.3. `cdd_execute_op`
-   **Purpose:** The workhorse function. Performs all data and metadata operations.
-   **`handle`:** A valid context handle.
-   **`json_op_request`:** A JSON string detailing the operation. See Section 4.3.
-   **`input_data_ptr` / `input_data_bytes`:** Used for `Store` operations. The raw binary data to be written. Can be `NULL` if not needed.
-   **`output_data_ptr` / `max_output_data_bytes`:** Used for `Load` operations. The caller-allocated buffer where decoded data will be written. Can be `NULL` if not needed.
-   **`json_op_response` / `max_json_response_bytes`:** Caller-allocated buffer for the JSON result. A response is *always* generated. If this buffer is too small to hold the full response, an error is returned.
-   **Return Value:** `0` on success. A negative error code on failure. The JSON response provides granular details for both success and failure cases.

#### 3.3.4. `cdd_error_message`
-   **Purpose:** A utility function for FFI clients to get human-readable error descriptions.
-   **Implementation Note:** This will be a simple `switch` statement mapping the error codes defined in Section 3.4 to static strings.

### 3.4. Error Handling and Status Codes
The FFI will use the following `int64_t` error codes. This list can be expanded.

| Code | Name | Description |
| :--- | :--- | :--- |
| 0 | `CDD_SUCCESS` | Operation completed successfully. |
| -1 | `CDD_ERROR_UNKNOWN` | An unspecified internal error occurred. |
| -2 | `CDD_ERROR_INVALID_JSON` | The provided JSON string was malformed or failed validation. |
| -3 | `CDD_ERROR_INVALID_HANDLE` | The provided context handle is not valid or has been destroyed. |
| -4 | `CDD_ERROR_OPERATION_FAILED`| The operation was valid but failed during execution (e.g., I/O error, compression failure). |
| -5 | `CDD_ERROR_RESPONSE_BUFFER_TOO_SMALL`| The provided `json_op_response` buffer is too small for the result. |
| -6 | `CDD_ERROR_INVALID_ARGUMENT`| A function argument was invalid (e.g., null pointer, size mismatch). |
| -7 | `CDD_ERROR_RESOURCE_UNAVAILABLE`| A required resource could not be accessed (e.g., file not found, permission denied). |

### 3.5. Data Types and Conventions
-   **Strings:** All `const char*` strings are assumed to be UTF-8 encoded and are not required to be null-terminated, as their length is always provided.
-   **Pointers:** `input_data_ptr` and `output_data_ptr` are treated as opaque `void*` pointers. Their interpretation is dictated by the `dtype` specified in the operation's JSON.

---

## 4. The JSON-RPC Protocol

This section defines the "language" used to communicate with the FFI.

### 4.1. General Schema Principles

#### 4.1.1. Versioning
All request JSON objects MUST contain a top-level `"api_version": "1.0"` key-value pair. This allows for future, non-breaking changes. The library will reject requests with an unsupported version.

#### 4.1.2. Request and Response Structure
-   **Requests:** Define a single action to be taken.
-   **Responses:** Are always generated for every `execute_op` call. They contain a top-level `"status"` field (`"Success"` or `"Error"`) and a payload.

### 4.2. Context Creation (`cdd_context_create`) JSON Schema
This JSON configures the session.

**Schema:**
```json
{
  "type": "object",
  "properties": {
    "api_version": { "const": "1.0" },
    "backend": {
      "type": "object",
      "properties": {
        "type": { "enum": ["File", "Memory"] },
        "path": { "type": "string" },
        "mode": { "enum": ["Read", "WriteAppend", "WriteTruncate"] },
        "name": { "type": "string" }
      },
      "required": ["type"],
      "if": { "properties": { "type": { "const": "File" } } },
      "then": { "required": ["path", "mode"] },
      "if": { "properties": { "type": { "const": "Memory" } } },
      "then": { "required": ["name", "mode"] }
    },
    "writer_options": {
      "type": "object",
      "properties": {
        "chunk_offsets_block_capacity": { "type": "integer", "minimum": 1 },
        "user_metadata_base64": { "type": "string" }
      }
    }
  },
  "required": ["api_version", "backend"]
}
```

**Example (Create a new file for writing):**
```json
{
  "api_version": "1.0",
  "backend": {
    "type": "File",
    "path": "/data/my_archive.cdd",
    "mode": "WriteTruncate"
  },
  "writer_options": {
    "user_metadata_base64": "eyJrZXkiOiAidmFsdWUifQ=="
  }
}
```

### 4.3. Operation Execution (`cdd_execute_op`) JSON Schemas

#### 4.3.1. **Store Operations**
These operations write data from the `input_data_ptr` into the context's backend.

##### 4.3.1.1. `op_type: "StoreChunk"`
Writes the entire `input_data_ptr` buffer as a single chunk.

**Schema:**
```json
{
  "op_type": "StoreChunk",
  "data_spec": { /* see 4.3.1.3 */ },
  "encoding": { /* see 4.3.1.3 */ }
}
```

**Example:**
```json
{
  "api_version": "1.0",
  "op_type": "StoreChunk",
  "data_spec": {
    "dtype": "INT64",
    "shape": [10000]
  },
  "encoding": {
    "codec": "TEMPORAL_1D_SIMD_I64_DELTA",
    "zstd_level": 5,
    "flags": ["LITTLE_ENDIAN"]
  }
}
```

##### 4.3.1.2. `op_type: "StoreArray"`
Treats `input_data_ptr` as a large array and splits it into multiple chunks before writing.

**Schema:**
```json
{
  "op_type": "StoreArray",
  "data_spec": { /* see 4.3.1.3 */ },
  "encoding": { /* see 4.3.1.3 */ },
  "chunking_strategy": {
    "type": "object",
    "properties": {
      "strategy": { "enum": ["ByCount", "Manual"] },
      // -- For strategy: "ByCount" --
      "rows_per_chunk": { "type": "integer", "minimum": 1 },
      // -- For strategy: "Manual" --
      "boundaries": { "type": "array", "items": { "type": "integer" } }
    },
    "required": ["strategy"]
  }
}
```
-   **`ByCount`:** Splits the array into chunks each containing `rows_per_chunk` rows (from the first dimension of the shape).
-   **`Manual`:** Provides the exact row indices that start each new chunk. `[0, 100, 250]` would create two chunks: rows 0-99 and rows 100-249.

##### 4.3.1.3. Common `encoding` and `data_spec` Blocks
These blocks are used by all store operations.

**`data_spec`:** Describes the raw data in the `input_data_ptr`.
```json
"data_spec": {
  "dtype": "FLOAT32" | "INT64" | "UINT8" | ..., // Matches DType enum
  "shape": [100, 50, 3] // Array of integers
}
```

**`encoding`:** Describes how the data should be processed and stored.
```json
"encoding": {
  "codec": "RAW" | "ZSTD_COMPRESSED" | "OKX_OB_SIMD_F32" | ..., // Matches ChunkDataType enum
  "zstd_level": 3,   // Optional, integer from 1-22. Used by all codecs that have a ZSTD final stage.
  "flags": ["LITTLE_ENDIAN"] // Optional array of strings from ChunkFlags enum.
}
```

#### 4.3.2. **Load Operations**

##### 4.3.2.1. `op_type: "LoadChunks"`
Reads one or more chunks from the backend, decodes them, and writes the concatenated raw data into `output_data_ptr`.

**Schema:**
```json
{
  "op_type": "LoadChunks",
  "selection": { /* see 4.3.2.2 */ },
  "validation": { /* see 4.3.2.2 */ } // Optional
}
```

**Example:**
```json
{
  "api_version": "1.0",
  "op_type": "LoadChunks",
  "selection": {
    "type": "Indices",
    "indices": [0, 1, 10]
  },
  "validation": {
    "expected_codec": "OKX_OB_SIMD_F16_AS_F32",
    "expected_dtype": "FLOAT32"
  }
}
```

##### 4.3.2.2. The `selection` and `validation` Blocks
**`selection`:** Specifies which chunks to load.
```json
"selection": {
  "type": "All" | "Indices" | "Range",
  // -- For type: "Indices" --
  "indices": [0, 5, 10], // Array of chunk indices
  // -- For type: "Range" --
  "start_index": 0,
  "count": 20
}
```

**`validation`:** Optional block to assert properties of the loaded chunks. If any assertion fails, the operation fails with an error.
```json
"validation": {
  "expected_codec": "OKX_OB_SIMD_F32", // A single codec string
  "expected_dtype": "FLOAT32" // A single dtype string
}
```

#### 4.3.3. **Metadata and Inspection Operations**

##### 4.3.3.1. `op_type: "Inspect"`
Retrieves metadata about the entire resource. Does not use `input` or `output` data pointers. The result is written to the JSON response.

**Schema:**
```json
{ "op_type": "Inspect" }
```

#### 4.3.3.2. `op_type: "GetUserMetadata"`
Retrieves only the user-defined metadata blob.

**Schema:**
```json
{ "op_type": "GetUserMetadata" }
```

#### 4.3.3.3. `op_type: "SetUserMetadata"`
Updates the user metadata in a file opened in a Write mode.

**Schema:**
```json
{
  "op_type": "SetUserMetadata",
  "user_metadata_base64": "..."
}
```

#### 4.3.4. **Control Operations**

##### 4.3.4.1. `op_type: "Flush"`
For contexts opened in a Write mode, this operation ensures all pending data and metadata (like the chunk index) are written to the backend.

**Schema:**
```json
{ "op_type": "Flush" }
```

### 4.4. The JSON Response Schema
A response is *always* generated and written to `json_op_response`.

#### 4.4.1. Success Response Structure
```json
{
  "status": "Success",
  "result": {
    // Operation-specific payload
    // Example for LoadChunks:
    "bytes_written_to_output": 60000,
    "final_shape": [100, 50, 3],
    "dtype": "FLOAT32",

    // Example for Inspect:
    "file_header": { "version": 1, ... },
    "total_chunks": 50,
    "chunk_summaries": [
        { "index": 0, "shape": [10, 50, 3], "dtype": "FLOAT32", "codec": "OKX_OB_SIMD_F32" },
        ...
    ]
  }
}
```

#### 4.4.2. Error Response Structure
```json
{
  "status": "Error",
  "error": {
    "code_name": "INVALID_JSON", // String representation of the FFI error code
    "code_value": -2,
    "message": "A detailed, human-readable error message.",
    "context": {
      "operation": "StoreChunk",
      "chunk_index": 5 // Optional, if applicable
    }
  }
}
```

---
*Continuation of Architectural Design Document: The Cryptodd FFI Gateway*

---

## 5. C++ Core Implementation Strategy

This section details the internal C++ architecture that powers the FFI gateway. The design emphasizes object-oriented principles, RAII, and clear separation of concerns to manage complexity.

### 5.1. The `CddContext` Class: The Heart of the Session
The `CddContext` is the central C++ object that encapsulates all state and resources for a single, active session with a data backend. An instance of this class is created by `cdd_context_create` and destroyed by `cdd_context_destroy`.

**Header Sketch (`cdd_context.h`):**
```cpp
#include "data_io/data_reader.h"
#include "data_io/data_writer.h"
#include "codecs/orderbook_simd_codec.h"
#include "codecs/temporal_1d_simd_codec.h"
#include "codecs/temporal_2d_simd_codec.h"
#include <variant>

namespace cryptodd::ffi {

class CddContext {
public:
    // Factory function to create a context from JSON configuration
    static std::expected<std::unique_ptr<CddContext>, std::string> create(const nlohmann::json& config);

    // Main execution entry point for this context
    std::expected<nlohmann::json, std::string> execute_operation(
        const nlohmann::json& op_request,
        std::span<const std::byte> input_data,
        std::span<std::byte> output_data
    );
    
    // Explicitly non-copyable and non-movable
    CddContext(const CddContext&) = delete;
    CddContext& operator=(const CddContext&) = delete;

private:
    // Private constructor, use factory
    CddContext(std::unique_ptr<storage::IStorageBackend> backend,
               std::unique_ptr<DataReader> reader,
               std::unique_ptr<DataWriter> writer);

    // Backends
    std::unique_ptr<storage::IStorageBackend> backend_;
    std::unique_ptr<DataReader> reader_; // Null if in write mode
    std::unique_ptr<DataWriter> writer_; // Null if in read mode

    // Reusable workspaces to avoid re-allocation
    OrderbookSimdCodecWorkspace orderbook_workspace_;
    Temporal1dSimdCodecWorkspace temporal_1d_workspace_;
    Temporal2dSimdCodecWorkspace temporal_2d_workspace_;

    // --- State for Temporal Codecs ---
    // This state is crucial for correct sequential processing of temporal chunks.
    int64_t last_element_i64_{0};
    float   last_element_f32_{0.0f};
    memory::vector<int64_t> last_row_i64_;
    memory::vector<float>   last_row_f32_;

    // Friends to allow handlers access to state
    friend class StoreChunkHandler;
    friend class LoadChunksHandler;
    // ... other handlers
};

} // namespace cryptodd::ffi
```
-   **Ownership:** The context owns the `IStorageBackend`, the `DataReader`/`Writer`, and all workspaces. Its destructor will automatically trigger the cleanup of these resources, ensuring files are flushed and closed correctly via RAII.
-   **State:** It serves as the single source of truth for the `prev_element`/`prev_row` state required by temporal codecs, ensuring continuity across multiple `execute_op` calls on the same handle.

### 5.2. Global Handle Management
A global, thread-safe map is used to associate the opaque `intptr_t` handles with their corresponding `CddContext` C++ objects.

**Implementation (`cdd_c_api.cpp`):**
```cpp
#include <unordered_map>
#include <mutex>
#include <atomic>
#include "cdd_context.h"

namespace {
    std::mutex g_context_map_mutex;
    std::unordered_map<intptr_t, std::unique_ptr<cryptodd::ffi::CddContext>> g_contexts;
    std::atomic<intptr_t> g_next_handle{1}; // Start handles at 1, as 0 can be a sentinel value
}

// Helper to safely access the context map
template<typename Func>
auto with_context(intptr_t handle, Func&& func) -> decltype(func(std::declval<cryptodd::ffi::CddContext&>())) {
    std::lock_guard lock(g_context_map_mutex);
    auto it = g_contexts.find(handle);
    if (it == g_contexts.end()) {
        // Handle not found
        throw std::runtime_error("Invalid context handle");
    }
    return func(*(it->second));
}
```
-   **Thread Safety:** A `std::mutex` protects all access to the global map, preventing race conditions during context creation, destruction, or lookup.
-   **Handle Generation:** A `std::atomic<intptr_t>` is used to generate unique, monotonically increasing handles in a thread-safe manner.

### 5.3. FFI Function Implementations
The public C functions are thin, exception-safe wrappers around the C++ core logic.

**Implementation (`cdd_c_api.cpp`):**
```cpp
int64_t cdd_context_create(const char* json_config, size_t config_len, intptr_t* out_handle) {
    if (!json_config || !out_handle) return CDD_ERROR_INVALID_ARGUMENT;

    try {
        auto config = nlohmann::json::parse(std::string_view(json_config, config_len));
        auto context_result = cryptodd::ffi::CddContext::create(config);
        if (!context_result) {
            // Handle creation failure (e.g., file not found)
            // We need a way to communicate this error back. For now, we'll throw and catch.
             throw std::runtime_error(context_result.error());
        }
        
        intptr_t handle = g_next_handle.fetch_add(1);
        {
            std::lock_guard lock(g_context_map_mutex);
            g_contexts[handle] = std::move(*context_result);
        }
        *out_handle = handle;
        return CDD_SUCCESS;

    } catch (const nlohmann::json::parse_error& e) {
        // Specific error for JSON parsing
        return CDD_ERROR_INVALID_JSON;
    } catch (const std::exception& e) {
        // Generic error
        return CDD_ERROR_UNKNOWN; // Log e.what() internally
    }
}

int64_t cdd_execute_op(...) {
    // 1. Basic argument validation (pointers, sizes)
    if (!handle || !json_op_request || !json_op_response) return CDD_ERROR_INVALID_ARGUMENT;

    std::string response_str;
    try {
        auto request_json = nlohmann::json::parse(std::string_view(json_op_request, request_len));
        
        // Use the helper to find the context and execute the operation
        auto response_json_result = with_context(handle, [&](auto& context) {
            return context.execute_operation(
                request_json,
                {static_cast<const std::byte*>(input_data_ptr), (size_t)input_data_bytes},
                {static_cast<std::byte*>(output_data_ptr), (size_t)max_output_data_bytes}
            );
        });

        nlohmann::json final_response;
        if (response_json_result) {
            final_response = {{"status", "Success"}, {"result", *response_json_result}};
        } else {
             final_response = {
                {"status", "Error"},
                {"error", {
                    {"code_name", "OPERATION_FAILED"},
                    {"code_value", CDD_ERROR_OPERATION_FAILED},
                    {"message", response_json_result.error()}
                }}
            };
        }
        response_str = final_response.dump();

    } catch (const std::exception& e) {
        // Catch exceptions from JSON parsing, invalid handle, etc.
        // Construct an error JSON response
        nlohmann::json error_response = { /* ... */ };
        response_str = error_response.dump();
        // Fall through to write the response, but we'll return an error code
    }

    // Write the response to the output buffer
    if (response_str.length() >= max_json_response_bytes) {
        // We must include the null terminator in the check if the caller expects it
        return CDD_ERROR_RESPONSE_BUFFER_TOO_SMALL;
    }
    memcpy(json_op_response, response_str.c_str(), response_str.length() + 1);

    // Determine the final return code
    // A bit more logic needed here to parse the final_response status and return the right code
    // For simplicity:
    // if (final_response["status"] == "Success") return CDD_SUCCESS; else return CDD_ERROR_...;
    return 0; // Simplified for now
}
```

### 5.4. The Operation Dispatcher and Handler Pattern
To avoid a massive `if/else if` block in `CddContext::execute_operation`, we use a handler (or strategy) pattern.

**Interface (`operation_handler.h`):**
```cpp
class IOperationHandler {
public:
    virtual ~IOperationHandler() = default;
    virtual std::expected<nlohmann::json, std::string> execute(
        CddContext& context,
        const nlohmann::json& op_request,
        std::span<const std::byte> input_data,
        std::span<std::byte> output_data
    ) = 0;
};
```

**Dispatcher Logic in `CddContext::execute_operation`:**
```cpp
std::expected<nlohmann::json, std::string> CddContext::execute_operation(...) {
    const std::string op_type = op_request.value("op_type", "");
    
    std::unique_ptr<IOperationHandler> handler;
    if (op_type == "StoreChunk") {
        handler = std::make_unique<StoreChunkHandler>();
    } else if (op_type == "LoadChunks") {
        handler = std::make_unique<LoadChunksHandler>();
    } else if (op_type == "Inspect") {
        handler = std::make_unique<InspectHandler>();
    } else {
        return std::unexpected("Unknown or missing op_type");
    }

    return handler->execute(*this, op_request, input_data, output_data);
}
```
This pattern makes the system highly extensible. To add a new operation, one simply needs to implement a new `IOperationHandler` class and add one line to the dispatcher.

### 5.5. State Management for Temporal Codecs
This is a critical implementation detail handled transparently inside the operation handlers.

**Example `StoreChunkHandler` Logic:**
```cpp
// Inside StoreChunkHandler::execute
// ... after parsing JSON and validating input ...

auto codec_type = parse_codec_from_json(op_request["encoding"]["codec"]);
auto dtype = parse_dtype_from_json(op_request["data_spec"]["dtype"]);

if (codec_type == ChunkDataType::TEMPORAL_1D_SIMD_I64_DELTA && dtype == DType::INT64) {
    auto compressor = std::make_unique<ZstdCompressor>(...);
    Temporal1dSimdCodec codec(std::move(compressor));
    
    auto data_span = std::span(reinterpret_cast<const int64_t*>(input_data.data()), input_data.size_bytes() / sizeof(int64_t));

    // 1. READ state from context
    int64_t prev_element = context.last_element_i64_;
    
    // 2. EXECUTE encoding
    auto encoded_result = codec.encode64_Delta(data_span, prev_element, context.temporal_1d_workspace_);
    if (!encoded_result) return std::unexpected(encoded_result.error());

    // 3. UPDATE state in context
    if (!data_span.empty()) {
        context.last_element_i64_ = data_span.back();
    }
    
    // 4. CREATE and WRITE chunk
    Chunk chunk;
    // ... set chunk properties, move encoded_result->data() into chunk ...
    context.writer_->append_chunk(chunk);
}
// ... other codec handlers ...
```
The same read-execute-update pattern applies to `LoadChunksHandler` to ensure correct reconstruction of temporal streams across multiple, separate calls.

### 5.6. Workspace and Resource Management
Workspaces are owned by the `CddContext`. Handlers request them from the context.

```cpp
// Inside a handler that needs a workspace
auto& workspace = context.getTemporal2dWorkspace(); // A getter on CddContext
workspace.ensure_capacity(required_elements);
// ... use workspace ...
```
This ensures that the large, aligned memory buffers for SIMD operations are allocated only once per context and reused, providing a significant performance benefit.

### 5.7. Error Propagation: `std::expected` to FFI Error Codes
The internal C++ code will heavily use `std::expected<T, std::string>` for functions that can fail, such as I/O or decompression.
-   **Low-Level Functions:** `DataReader::get_chunk`, `ICompressor::decompress`, etc., return `std::expected`.
-   **Operation Handlers:** Propagate these expecteds up. If an operation fails, the handler returns an `std::unexpected` containing a descriptive error string.
-   **`CddContext::execute_operation`:** Catches the `std::unexpected` from the handler.
-   **FFI Wrapper (`cdd_execute_op`):** This top-level C function's `try...catch` block is the final safety net. An error from the context is caught, and the `std::string` message is used to populate the `"error"` field of the JSON response, while the function itself returns a generic `CDD_ERROR_OPERATION_FAILED` code.

## 6. Language Binding Strategies

The C FFI is the stable foundation, but it is not intended for direct end-user consumption. High-level, idiomatic wrappers must be provided for target languages.

### 6.1. Python (pybind11) Binding

**Strategy:** Create a Python class `cdd.File` that encapsulates the context handle and provides a user-friendly, Pythonic interface.

#### 6.1.1. The Wrapper Class (`cdd.File`)
```python
# python/cryptodd_api.py
import json
import numpy as np
from . import _c_api # The raw pybind11/CFFI wrapper around the C functions

class CddFile:
    def __init__(self, path: str, mode: str = 'r', **kwargs):
        self._handle = 0
        config = {
            "api_version": "1.0",
            "backend": {"type": "File", "path": path, "mode": mode.capitalize()},
            "writer_options": kwargs
        }
        # Call the FFI to create the context
        self._handle = _c_api.context_create(json.dumps(config))

    def close(self):
        if self._handle:
            _c_api.context_destroy(self._handle)
            self._handle = 0
    
    # ... methods for store, load, inspect ...

    def __del__(self):
        # Ensure cleanup even if the user forgets to call close()
        self.close()
```

#### 6.1.2. Resource Management with `__enter__`/`__exit__`
This makes the class compatible with Python's `with` statement, which is the standard for resource management.

```python
# Inside the CddFile class
def __enter__(self):
    return self

def __exit__(self, exc_type, exc_val, exc_tb):
    self.close()

# User code:
with cdd.File("data.cdd", "w") as f:
    f.store(my_array, ...)
# f.close() is automatically called here, even if an exception occurs.
```

#### 6.1.3. Implementing `store()` and `load()`
These methods will construct the JSON request and manage the data buffers.

```python
# Inside the CddFile class
def store_chunk(self, data: np.ndarray, codec: str, **kwargs):
    if not data.flags['C_CONTIGUOUS']:
        data = np.ascontiguousarray(data)

    request = {
        "api_version": "1.0",
        "op_type": "StoreChunk",
        "data_spec": {"dtype": str(data.dtype).upper(), "shape": list(data.shape)},
        "encoding": {"codec": codec, **kwargs}
    }
    
    _c_api.execute_op(self._handle, json.dumps(request), data) # Pass numpy array directly
    
def load_chunks(self, indices: list[int], validation: dict = None) -> np.ndarray:
    # 1. Inspect first to get required buffer size, shape, and dtype
    inspect_result = self.inspect()
    # ... logic to calculate total shape and size from chunk summaries ...
    
    # 2. Allocate output numpy array
    output_array = np.empty(shape=total_shape, dtype=np.dtype(dtype_str))
    
    # 3. Build load request
    request = {
        "api_version": "1.0",
        "op_type": "LoadChunks",
        "selection": {"type": "Indices", "indices": indices}
    }
    if validation:
        request["validation"] = validation
        
    # 4. Call FFI, passing the numpy array's buffer
    response = _c_api.execute_op(self._handle, json.dumps(request), output_buffer=output_array)
    
    # The data is now in output_array
    return output_array
```

#### 6.1.4. Buffer Protocol and Data Marshalling
The `pybind11` wrapper for `cdd_execute_op` will be designed to automatically handle NumPy arrays. When a `numpy.ndarray` is passed, pybind11 can access its buffer information (`py::buffer_info`) to get the raw pointer, size, and dimensions, which are then passed to the underlying C function.

### 6.2. C# (P/Invoke) Binding

The principles are identical to Python: create a high-level class that safely wraps the low-level C FFI.

#### 6.2.1. The Wrapper Class (`CddFile` implementing `IDisposable`)
`IDisposable` is the .NET equivalent of Python's context manager protocol.

```csharp
public class CddFile : IDisposable
{
    private IntPtr _handle;

    public CddFile(string path, string mode)
    {
        // Build JSON config...
        long status = CddApi.cdd_context_create(jsonConfig, (UIntPtr)jsonConfig.Length, out _handle);
        if (status != 0) throw new CddException(status);
    }
    
    public void Dispose()
    {
        if (_handle != IntPtr.Zero)
        {
            CddApi.cdd_context_destroy(_handle);
            _handle = IntPtr.Zero;
        }
        GC.SuppressFinalize(this);
    }
    
    ~CddFile() { Dispose(); } // Finalizer for safety
}

// User code:
using (var f = new CddFile("data.cdd", "w"))
{
    f.StoreChunk(myArray, ...);
} // f.Dispose() is automatically called here
```

#### 6.2.2. Safe Handles for Context Management
For maximum robustness, the `IntPtr _handle` can be wrapped in a `Microsoft.Win32.SafeHandles.SafeHandleZeroOrMinusOneIsInvalid` derived class. This provides stronger guarantees against handle leaks and recycling attacks.

#### 6.2.3. Data Marshalling with `Marshal`
Getting data to/from the C API from managed C# arrays requires "pinning" the memory so the garbage collector doesn't move it during the P/Invoke call.

```csharp
// Inside CddFile.StoreChunk
public void StoreChunk<T>(T[,] data, ...) where T : unmanaged
{
    // ... build JSON request ...
    
    var gcHandle = GCHandle.Alloc(data, GCHandleType.Pinned);
    try
    {
        IntPtr dataPtr = gcHandle.AddrOfPinnedObject();
        long dataBytes = (long)data.Length * Marshal.SizeOf<T>();
        
        // Allocate buffers for JSON response
        IntPtr responsePtr = Marshal.AllocHGlobal(4096);

        long status = CddApi.cdd_execute_op(
            _handle,
            requestJson, (UIntPtr)requestJson.Length,
            dataPtr, dataBytes,
            IntPtr.Zero, 0, // No output data buffer
            responsePtr, 4096
        );
        
        // Check status, parse response JSON, etc.
    }
    finally
    {
        gcHandle.Free();
        Marshal.FreeHGlobal(responsePtr);
    }
}
```

### 6.3. Direct C/C++ Usage
Direct usage follows the pattern of creating a context, executing operations, and destroying the context, with manual memory and buffer management. It is the most verbose but also provides the most control.

## 7. In-Depth Design Rationale & Q&A

This section explicitly addresses the key architectural decisions made during the design process.

### 7.1. Q1: Why a stateful FFI (context handle) instead of a simpler stateless one?
A stateful design is required for both correctness and performance. The `DataWriter` object, which manages the file format's indices and offsets, is inherently stateful. Appending multiple chunks requires this state to be preserved between calls. Furthermore, a stateful context allows for expensive resources like file handles and large memory workspaces for codecs to be initialized once and reused across many operations, which is vastly more efficient than a stateless model that would re-initialize these on every call.

### 7.2. Q2: Why JSON instead of a binary format like Protobuf or FlatBuffers?
JSON was chosen for its unparalleled combination of:
-   **Human Readability:** This is invaluable for debugging and logging.
-   **Universal Support:** Every modern language has excellent, built-in JSON support, making bindings easier to write.
-   **Flexibility & Extensibility:** Adding new, optional fields to a JSON schema is trivial and non-breaking.
    While a binary format might offer slightly lower parsing overhead, this is negligible in the context of a library whose primary work involves disk I/O and CPU-intensive compression/decompression. The developer ergonomics and flexibility of JSON far outweigh the micro-optimizations of a binary format for this use case.

### 7.3. Q3: Why was the zero-copy "view" for Python rejected in favor of a copy-on-load?
A true zero-copy load would require the C++ core to allocate and own the memory for the decoded data, with the Python layer merely holding a "view" onto it via a `py::capsule`. While technically feasible, this approach was rejected for several key reasons:
-   **Architectural Impurity:** It would require a special API path just for Python, breaking the unified FFI gateway model. The C API cannot safely return ownership of memory it allocates to a generic FFI client.
-   **Increased Complexity:** Managing the lifetime of C++-allocated memory that is tied to a Python garbage-collected object is complex and a common source of bugs.
-   **Sufficient Performance:** The performance-critical parts of a load operation are disk I/O and the SIMD decoding. The final `memcpy` of the result into a pre-allocated NumPy array is extremely fast and is not the primary bottleneck.
    Therefore, the architectural simplicity, safety, and maintainability of the unified, copy-on-load model were deemed more valuable than the marginal performance gain of a complex, special-cased zero-copy implementation.

### 7.4. Q4: How is thread safety handled?
The threading model is simple and explicit:
-   The **FFI Gateway itself is thread-safe.** The global map that stores context handles is protected by a mutex, allowing different threads to create, destroy, and look up contexts concurrently.
-   A **`CddContext` instance is NOT thread-safe.** It is designed to be used by a single thread at a time. This is a standard and well-understood pattern.
    **The contract for users is:** Do not share a single context handle across multiple threads without implementing your own external locking. For parallel processing, the recommended approach is for each thread to create its own `CddContext`.

### 7.5. Q5: How will the system handle endianness?
The design adopts a pragmatic approach:
-   The library operates in the native endianness of the host machine.
-   Chunks can be written with a `LITTLE_ENDIAN` flag in their metadata.
-   **On a little-endian machine (the vast majority of modern systems), this is transparent.**
-   **On a big-endian machine, the implementation of byte-swapping is explicitly out of scope for version 1.0.** If the library, running on a big-endian machine, is asked to read a chunk flagged as `LITTLE_ENDIAN` or write a chunk with that flag, it will return an error. This simplifies the initial implementation significantly while still correctly identifying and preventing data corruption.

### 7.6. Q6: How is the API designed for future extensibility?
Extensibility is built into the design at multiple levels:
-   **New Operations:** A new `op_type` can be added to the JSON schema. This requires implementing a new `IOperationHandler` class in the C++ core and adding it to the dispatcher. Existing clients are unaffected.
-   **New Codecs:** A new codec can be added to the `ChunkDataType` enum and the `encoding.codec` JSON field. This requires implementing the codec logic and adding it to the store/load dispatchers.
-   **New Options:** New optional fields can be added to the JSON schemas. As long as they are not required, existing clients will continue to work.
-   **API Versioning:** The mandatory `api_version` field in all requests allows the library to handle breaking changes in the future by supporting multiple versions of the request/response logic simultaneously or by cleanly rejecting requests for unsupported versions.

## 8. Implementation Plan

The development will be executed in phased, deliverable stages.

### 8.1. Phase 1: Core FFI and Context Management
1.  Implement the final C API header (`cryptodd_c_api.h`).
2.  Implement the global handle map with its mutex.
3.  Implement the skeleton `CddContext` class.
4.  Implement `cdd_context_create` and `cdd_context_destroy`.
5.  Implement a basic `cdd_execute_op` that can handle a simple `op_type: "Ping"` for testing the round-trip.
6.  **Deliverable:** A linkable library that can manage context lifetimes.

### 8.2. Phase 2: Store Operations
1.  Implement the `IOperationHandler` interface and the operation dispatcher in `CddContext`.
2.  Implement `StoreChunkHandler`.
    -   Add support for `RAW` and `ZSTD_COMPRESSED` codecs first.
    -   Integrate the temporal state management into `CddContext`.
    -   Add support for `TEMPORAL_1D...` and `OKX_OB...` codecs.
3.  Implement `StoreArrayHandler` with the `ByCount` chunking strategy.
4.  **Deliverable:** Ability to create fully-featured `.cdd` files from in-memory arrays.

### 8.3. Phase 3: Load and Inspect Operations
1.  Implement `InspectHandler`.
2.  Implement `LoadChunksHandler`.
    -   Add support for decoding all codecs implemented in Phase 2.
    -   Implement the `selection` and `validation` logic.
3.  Implement handlers for `GetUserMetadata`, `SetUserMetadata`, and `Flush`.
4.  **Deliverable:** Full round-trip capability (write, inspect, read) and metadata manipulation.

### 8.4. Phase 4: Python and C# Bindings
1.  Develop the `pybind11` wrapper, creating the high-level `cdd.File` class.
2.  Implement comprehensive Python unit and integration tests using NumPy.
3.  Develop the C# P/Invoke wrapper and the `CddFile : IDisposable` class.
4.  Implement comprehensive C# unit and integration tests.
5.  **Deliverable:** A fully usable library for the primary target languages.

### 8.5. Phase 5: Documentation and Examples
1.  Write detailed API documentation for the C FFI.
2.  Write user-facing documentation for the Python and C# libraries.
3.  Create a repository of example use cases for each language.
4.  Perform benchmarking and add performance notes to the documentation.
5.  **Deliverable:** A production-ready library with full supporting materials.