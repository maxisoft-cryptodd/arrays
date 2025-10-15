#pragma once

#include "../file_format/cdd_file_format.h" // For Chunk, ChunkDataType, DType, etc.
#include "codec_error.h"      // For CodecError
#include "../codecs/zstd_compressor.h"      // For ZstdCompressor::DEFAULT_COMPRESSION_LEVEL
#include <expected>
#include <memory>
#include <span>
#include <vector>

namespace cryptodd
{
// Forward declaration
class Chunk;

/**
 * @class DataCompressor
 * @brief Provides a high-performance, thread-safe interface to encode and compress raw data into Chunks.
 *
 * This class is the encoding counterpart to a DataExtractor. It takes raw data spans and
 * state information (e.g., the previous element or snapshot), applies the specified SIMD-accelerated
 * encoding and compression based on a target ChunkDataType, and returns a fully formed, compressed Chunk.
 *
 * @section usage Usage
 * The compressor's primary API is a set of overloaded `compress_chunk` methods. You provide the raw
 * data, the target `ChunkDataType` you wish to produce, any required state, and a compression level.
 * The class handles the internal dispatch to the correct codec.
 *
 * @section performance Performance and Thread Safety
 * The class is thread-safe. However, for maximum performance in highly concurrent
 * applications, it is **strongly recommended** to create and use one `DataCompressor`
 * instance per thread. This avoids contention on internal workspace buffers. If a single
 * instance is shared, access to these buffers will be serialized by mutexes.
 * Caching of codec instances is handled internally and is thread-safe.
 */
class DataCompressor
{
    struct Impl;
    std::unique_ptr<Impl> pimpl_;

  public:
    DataCompressor();
    ~DataCompressor();
    DataCompressor(DataCompressor&&) noexcept;
    DataCompressor& operator=(DataCompressor&&) noexcept;

    DataCompressor(const DataCompressor&) = delete;
    DataCompressor& operator=(const DataCompressor&) = delete;

    using ChunkResult = std::expected<std::unique_ptr<Chunk>, CodecError>;

    /**
     * @brief Compresses a raw byte span using Zstd. This is for simple, non-SIMD compression.
     * @param data The raw data to compress.
     * @param shape The shape of the original data.
     * @param dtype The data type of the original data.
     * @param level The Zstd compression level.
     * @return A Chunk containing the compressed data, or an error.
     */
    [[nodiscard]] ChunkResult compress_zstd(
        std::span<const std::byte> data,
        std::span<const uint32_t> shape,
        DType dtype,
        int level = ZstdCompressor::DEFAULT_COMPRESSION_LEVEL
    ) const;

    /**
     * @brief Encodes a 1D series of floats with a default (zero) initial state.
     */
    [[nodiscard]] ChunkResult compress_chunk(
        std::span<const float> data,
        ChunkDataType type,
        int level = ZstdCompressor::DEFAULT_COMPRESSION_LEVEL
    ) const;

    /**
     * @brief Encodes a 1D series of floats based on the specified temporal chunk type.
     * @param data The raw float data to encode.
     * @param type The target chunk type (e.g., TEMPORAL_1D_SIMD_F16_XOR_SHUFFLE_AS_F32).
     * @param prev_element The state from the previous chunk.
     * @param level The Zstd compression level.
     * @return A Chunk containing the encoded and compressed data, or an error.
     */
    [[nodiscard]] ChunkResult compress_chunk(
        std::span<const float> data,
        ChunkDataType type,
        float prev_element,
        int level = ZstdCompressor::DEFAULT_COMPRESSION_LEVEL
    ) const;
    
    /**
     * @brief Encodes a 1D series of int64s based on the specified temporal chunk type.
     * @param data The raw int64 data to encode.
     * @param type The target chunk type (e.g., TEMPORAL_1D_SIMD_I64_XOR).
     * @param prev_element The state from the previous chunk.
     * @param level The Zstd compression level.
     * @return A Chunk containing the encoded and compressed data, or an error.
     */
    [[nodiscard]] ChunkResult compress_chunk(
        std::span<const int64_t> data,
        ChunkDataType type,
        int64_t prev_element,
        int level = ZstdCompressor::DEFAULT_COMPRESSION_LEVEL
    ) const;

    /**
     * @brief Encodes a 1D series of int64s with a default (zero) initial state.
     */
    [[nodiscard]] ChunkResult compress_chunk(
        std::span<const int64_t> data,
        ChunkDataType type,
        int level = ZstdCompressor::DEFAULT_COMPRESSION_LEVEL
    ) const;

    /**
     * @brief Encodes a 2D/3D series of floats (Temporal 2D or Orderbook) based on the chunk type.
     * @param data The raw float data to encode, laid out in Structure-of-Arrays (SoA) format.
     * @param type The target chunk type (e.g., GENERIC_OB_SIMD_F16_AS_F32, TEMPORAL_2D_SIMD_F32).
     * @param shape The shape of the data. Must be 2D for Temporal 2D or 3D for Orderbook.
     * @param prev_state The state of the previous row or snapshot.
     * @param level The Zstd compression level.
     * @return A Chunk containing the encoded and compressed data, or an error.
     */
    [[nodiscard]] ChunkResult compress_chunk(
        std::span<const float> data,
        ChunkDataType type,
        std::span<const uint32_t> shape,
        std::span<const float> prev_state,
        int level = ZstdCompressor::DEFAULT_COMPRESSION_LEVEL
    ) const;
    
    /**
     * @brief Encodes a 2D series of int64s based on the specified temporal chunk type.
     * @param data The raw int64 data to encode, laid out in Structure-of-Arrays (SoA) format.
     * @param type The target chunk type (e.g., TEMPORAL_2D_SIMD_I64).
     * @param shape The 2D shape of the data.
     * @param prev_row The state of the previous row.
     * @param level The Zstd compression level.
     * @return A Chunk containing the encoded and compressed data, or an error.
     */
    [[nodiscard]] ChunkResult compress_chunk(
        std::span<const int64_t> data,
        ChunkDataType type,
        std::span<const uint32_t> shape,
        std::span<const int64_t> prev_row,
        int level = ZstdCompressor::DEFAULT_COMPRESSION_LEVEL
    ) const;
};

} // namespace cryptodd