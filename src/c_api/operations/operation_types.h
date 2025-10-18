#pragma once

#include "../file_format/cdd_file_format.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cryptodd::ffi {

// ==========================================================================
// Base Structures & Metadata
// ==========================================================================

struct OperationRequestBase {
    std::optional<std::string> client_key;
};

struct OperationResponseBase {
    std::optional<std::string> client_key;
};

struct OperationMetadata {
    std::string backend_type;
    std::string mode;
    uint64_t duration_us;
};

// ==========================================================================
// Common Data Structures
// ==========================================================================

struct DataSpec {
    DType dtype;
    std::vector<int64_t> shape;
};

struct EncodingSpec {
    ChunkDataType codec;
    std::vector<std::string> flags;
    std::optional<int> zstd_level;
};

struct ByCountChunking {
    int64_t rows_per_chunk;
};

using ChunkingStrategy = std::variant<ByCountChunking>;

// ==========================================================================
// Operation-Specific Structures
// ==========================================================================

// --- StoreChunk ---
struct StoreChunkRequest : OperationRequestBase {
    DataSpec data_spec;
    EncodingSpec encoding;
};

struct ChunkWriteDetails {
    size_t chunk_index;
    int64_t original_size;
    int64_t compressed_size;
    float compression_ratio;
};

struct StoreChunkResponse : OperationResponseBase {
    ChunkWriteDetails details;
    std::vector<int64_t> shape;
    int zstd_level;
    OperationMetadata metadata{};
};

// --- StoreArray ---
struct StoreArrayRequest : OperationRequestBase {
    DataSpec data_spec;
    EncodingSpec encoding;
    ChunkingStrategy chunking_strategy;
};

struct StoreArrayResponse : OperationResponseBase {
    int chunks_written;
    int64_t total_original_bytes;
    int64_t total_compressed_bytes;
    std::vector<ChunkWriteDetails> chunk_details;
    OperationMetadata metadata{};
};

// --- LoadChunks ---
struct AllSelection {};
struct IndicesSelection { std::vector<size_t> indices; };
struct RangeSelection { size_t start_index; size_t count; };
using ChunkSelection = std::variant<AllSelection, IndicesSelection, RangeSelection>;

struct LoadChunksRequest : OperationRequestBase {
    ChunkSelection selection;
};

struct LoadChunksResponse : OperationResponseBase {
    size_t bytes_written_to_output{};
    std::optional<std::vector<int64_t>> final_shape;
    OperationMetadata metadata{};
};

// --- Inspect ---
struct InspectRequest : OperationRequestBase {
    bool calculate_checksums = false; // For future enhancement
};

struct ChunkSummary {
    size_t index;
    std::vector<int64_t> shape;
    DType dtype;
    ChunkDataType codec;
    size_t encoded_size_bytes;
    size_t decoded_size_bytes;
};

struct FileHeaderInfo {
    uint32_t version;
    uint64_t index_block_offset;
    uint64_t index_block_size;
    std::string user_metadata_base64;
};

struct InspectResponse : OperationResponseBase {
    FileHeaderInfo file_header;
    size_t total_chunks;
    std::vector<ChunkSummary> chunk_summaries;
    OperationMetadata metadata{};
};

// --- Metadata Operations ---
struct GetUserMetadataRequest : OperationRequestBase {};
struct GetUserMetadataResponse : OperationResponseBase {
    std::string user_metadata_base64;
    OperationMetadata metadata{};
};

struct SetUserMetadataRequest : OperationRequestBase {
    std::string user_metadata_base64;
};
struct SetUserMetadataResponse : OperationResponseBase {
    std::string status;
    OperationMetadata metadata{};
};

// --- Flush ---
struct FlushRequest : OperationRequestBase {};
struct FlushResponse : OperationResponseBase {
    std::string status;
    OperationMetadata metadata{};
};

// --- Ping ---
struct PingRequest : OperationRequestBase {};
struct PingResponse : OperationResponseBase {
    std::string message;
    OperationMetadata metadata{};
};

} // namespace cryptodd::ffi
