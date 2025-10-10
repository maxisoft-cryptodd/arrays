// src/codecs/okx_ob_simd_codec.h

#pragma once

#include <vector>
#include <cstdint>
#include <span>
#include <array>

namespace cryptodd {

// Represents a single order book snapshot (50 levels * 3 features)
constexpr size_t OKX_OB_SNAPSHOT_FLOATS = 50 * 3;
using OkxSnapshot = std::array<float, OKX_OB_SNAPSHOT_FLOATS>;


class OkxObSimdCodec {
public:
    OkxObSimdCodec() = delete;

    /**
     * @brief Encodes a batch of OB snapshots using a high-performance SIMD pipeline.
     *
     * Pipeline: Temporal XOR (f32) -> Demote (f16) -> Byte Shuffle (f16) -> Zstd
     *
     * @param snapshots Span of contiguous snapshot data. The number of floats must be a multiple of OKX_OB_SNAPSHOT_FLOATS.
     * @param prev_snapshot The state of the snapshot preceding this batch, used for the first XOR delta.
     * @return A vector of compressed bytes.
     */
    static std::vector<uint8_t> encode(std::span<const float> snapshots, const OkxSnapshot& prev_snapshot);

    /**
     * @brief Decodes a batch of OB snapshots using a high-performance SIMD pipeline.
     *
     * Pipeline: Zstd -> Fused [Unshuffle(f16) + Promote(f32) + Prefix-XOR Scan]
     *
     * @param encoded_data The compressed data from a single chunk.
     * @param num_snapshots The number of snapshots contained in this chunk.
     * @param prev_snapshot The state of the snapshot preceding this batch, used to start the reconstruction.
     * @return A vector of decoded, reconstructed snapshot data in float32 format.
     */
    static std::vector<float> decode(std::span<const uint8_t> encoded_data, size_t num_snapshots, OkxSnapshot& prev_snapshot);
};

} // namespace cryptodd
