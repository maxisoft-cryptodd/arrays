#include <stdexcept>
#include <string>
#include <numeric>
#include <array>
#include <algorithm> // For std::copy_n

#include "zstd.h"
#include "okx_ob_simd_codec.h"

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "okx_ob_simd_codec.cpp" // This file includes itself
#include "hwy/foreach_target.h"

// Must come after foreach_target.h to avoid redefinition errors.
#include "hwy/highway.h"
#include "hwy/aligned_allocator.h"

// =================================================================================
// FORWARD DECLARATIONS for Highway's dynamic dispatch
// =================================================================================
namespace cryptodd::HWY_NAMESPACE {
    void TemporalXor(const float* HWY_RESTRICT current, const float* HWY_RESTRICT prev, float* HWY_RESTRICT out, size_t num_floats);
    void DemoteFloat32ToFloat16(const float* HWY_RESTRICT in, size_t num_floats, hwy::float16_t* HWY_RESTRICT out);
    void ShuffleFloat16(const hwy::float16_t* HWY_RESTRICT in, uint8_t* HWY_RESTRICT out, size_t num_f16);
    void UnshuffleAndReconstruct(const uint8_t* HWY_RESTRICT shuffled_in, float* HWY_RESTRICT out, size_t num_snapshots, OkxSnapshot& last_snapshot_state);
}

// =================================================================================
// SIMD IMPLEMENTATIONS (included multiple times by foreach_target.h)
// =================================================================================

HWY_BEFORE_NAMESPACE();
namespace cryptodd
{
namespace HWY_NAMESPACE{

namespace hn = hwy::HWY_NAMESPACE;


inline constexpr hn::ScalableTag<float> d32;
inline constexpr hn::ScalableTag<hwy::float16_t> d16;
inline constexpr hn::ScalableTag<uint32_t> du32;
inline constexpr hn::ScalableTag<uint16_t> du16;
inline constexpr hn::ScalableTag<uint8_t> du8;

using VF32 = hn::Vec<hn::ScalableTag<float>>;
using VU32 = hn::Vec<hn::ScalableTag<uint32_t>>;
using VF16 = hn::Vec<hn::ScalableTag<hwy::float16_t>>;
using VU16 = hn::Vec<hn::ScalableTag<uint16_t>>;

// --- ENCODING PIPELINE ---
void TemporalXor(const float* HWY_RESTRICT current, const float* HWY_RESTRICT prev, float* HWY_RESTRICT out, size_t num_floats) {
    for (size_t i = 0; i < num_floats; i += hn::Lanes(d32)) {
        const VF32 v_curr = hn::LoadU(d32, current + i);
        const VF32 v_prev = hn::LoadU(d32, prev + i);
        const VU32 v_curr_u32 = hn::BitCast(du32, v_curr);
        const VU32 v_prev_u32 = hn::BitCast(du32, v_prev);
        const VU32 v_xor_u32 = hn::Xor(v_curr_u32, v_prev_u32);
        const VF32 v_xor_f32 = hn::BitCast(d32, v_xor_u32);
        hn::StoreU(v_xor_f32, d32, out + i);
    }
}

void DemoteFloat32ToFloat16(const float* HWY_RESTRICT in, size_t num_floats, hwy::float16_t* HWY_RESTRICT out) {
    size_t i = 0;

#if HWY_TARGET != HWY_SCALAR
    // SIMD PATH START: Process full vectors. This block is excluded for the scalar target.
    const hn::Half<decltype(d16)> d16_half;
    const size_t f32_lanes = hn::Lanes(d32);
    const size_t f16_lanes = hn::Lanes(d16);

    for (; i + f16_lanes <= num_floats; i += f16_lanes) {
        // Load two vectors of f32, which together match the number of f16 lanes.
        const VF32 v_f32_lo = hn::LoadU(d32, in + i);
        const VF32 v_f32_hi = hn::LoadU(d32, in + i + f32_lanes);

        // Demote each f32 vector to a half-width f16 vector.
        auto v_f16_lo_half = hn::DemoteTo(d16_half, v_f32_lo);
        auto v_f16_hi_half = hn::DemoteTo(d16_half, v_f32_hi);

        // Combine the two half-width vectors into a full f16 vector.
        VF16 v_f16_full = hn::Combine(d16, v_f16_hi_half, v_f16_lo_half);
        hn::StoreU(v_f16_full, d16, out + i);
    }
    // SIMD PATH END
#endif

    // SCALAR/REMAINDER PATH: Process any remaining elements one by one.
    // For the HWY_SCALAR target, this loop will handle all elements.
    for (; i < num_floats; ++i) {
        out[i] = hwy::ConvertScalarTo<hwy::float16_t>(in[i]);
    }
}

// This pattern de-interleaves float16 values (viewed as uint16_t) into two separate
// planes of low bytes and high bytes. It is compatible with all SIMD targets (except HWY_SCALAR).
void ShuffleFloat16(const hwy::float16_t* HWY_RESTRICT in, uint8_t* HWY_RESTRICT out, size_t num_f16) {
    uint8_t* out_b0 = out;          // Plane for lower bytes
    uint8_t* out_b1 = out + num_f16; // Plane for higher bytes
#if HWY_TARGET == HWY_SCALAR
    // SCALAR PATH START
    for (size_t i = 0; i < num_f16; ++i) {
        const uint16_t val = hwy::BitCastScalar<uint16_t>(in[i]);
        out_b0[i] = static_cast<uint8_t>(val & 0xFF);
        out_b1[i] = static_cast<uint8_t>(val >> 8);
    }
    // SCALAR PATH END
#else
    // SIMD PATH START
    const hn::Repartition<uint8_t, decltype(du16)> d_u8_packed;
    const size_t lanes_u16 = hn::Lanes(du16);

    // Process two vectors' worth of data per loop because OrderedTruncate2To combines two vectors.
    const size_t step = 2 * lanes_u16;
    size_t i = 0;
    for (; i + step <= num_f16; i += step) {
        // Load two vectors of float16 and view them as uint16
        const VU16 v_in_a = hn::BitCast(du16, hn::LoadU(d16, in + i));
        const VU16 v_in_b = hn::BitCast(du16, hn::LoadU(d16, in + i + lanes_u16));

        // Isolate lower bytes (e.g., L0, L1, ...) and higher bytes (H0, H1, ...)
        const VU16 lo_mask = hn::Set(du16, 0x00FF);
        const VU16 v_lo_a = hn::And(v_in_a, lo_mask);
        const VU16 v_hi_a = hn::ShiftRight<8>(v_in_a);
        const VU16 v_lo_b = hn::And(v_in_b, lo_mask);
        const VU16 v_hi_b = hn::ShiftRight<8>(v_in_b);

        // Pack the 8-bit results from two uint16 vectors into a single uint8 vector.
        // This replaces the non-existent `PackU16ToU8` with the correct Highway function.
        auto packed_lo = hn::OrderedTruncate2To(d_u8_packed, v_lo_a, v_lo_b);
        auto packed_hi = hn::OrderedTruncate2To(d_u8_packed, v_hi_a, v_hi_b);

        // Store the packed byte planes.
        hn::StoreU(packed_lo, d_u8_packed, out_b0 + i);
        hn::StoreU(packed_hi, d_u8_packed, out_b1 + i);
    }

    // Remainder loop for SIMD path (if num_f16 is not a multiple of `step`)
    for (; i < num_f16; ++i) {
        const uint16_t val = hwy::BitCastScalar<uint16_t>(in[i]);
        out_b0[i] = static_cast<uint8_t>(val & 0xFF);
        out_b1[i] = static_cast<uint8_t>(val >> 8);
    }
    // SIMD PATH END
#endif
}


// --- DECODING PIPELINE ---

void UnshuffleAndReconstruct(const uint8_t* HWY_RESTRICT shuffled_in, float* HWY_RESTRICT out, size_t num_snapshots, OkxSnapshot& last_snapshot_state) {
    const size_t num_floats_per_snapshot = OKX_OB_SNAPSHOT_FLOATS;
    const size_t total_floats = num_snapshots * num_floats_per_snapshot;

    float* HWY_RESTRICT prev_snapshot_ptr = last_snapshot_state.data();

    for (size_t s = 0; s < num_snapshots; ++s) {
        float* HWY_RESTRICT current_out_ptr = out + s * num_floats_per_snapshot;
        const size_t base_idx_bytes = s * num_floats_per_snapshot;
#if HWY_TARGET == HWY_SCALAR
        // SCALAR PATH START
        for (size_t i = 0; i < num_floats_per_snapshot; ++i) {
            const uint8_t b0 = shuffled_in[base_idx_bytes + i];
            const uint8_t b1 = shuffled_in[total_floats + base_idx_bytes + i];
            const uint16_t u16_val = (static_cast<uint16_t>(b1) << 8) | b0;

            const auto f16_delta = hwy::BitCastScalar<hwy::float16_t>(u16_val);
            const float f32_delta = hwy::ConvertScalarTo<float>(f16_delta);
            const float prev_val = prev_snapshot_ptr[i];

            const uint32_t recon_u32 = hwy::BitCastScalar<uint32_t>(f32_delta) ^ hwy::BitCastScalar<uint32_t>(prev_val);
            current_out_ptr[i] = hwy::BitCastScalar<float>(recon_u32);
        }
        // SCALAR PATH END
#else
        // SIMD PATH START
        const size_t f32_lanes = hn::Lanes(d32);
        const size_t f16_lanes = hn::Lanes(d16);

        // Process a full f16 vector's worth of floats at a time.
        for (size_t i = 0; i < num_floats_per_snapshot; i += f16_lanes) {
            // Load bytes from the two separate planes (low bytes and high bytes).
            const hn::Rebind<uint8_t, decltype(d16)> d_u8_rebind;
            const auto v_b0 = hn::LoadU(d_u8_rebind, shuffled_in + base_idx_bytes + i);
            const auto v_b1 = hn::LoadU(d_u8_rebind, shuffled_in + total_floats + base_idx_bytes + i);

            // Interleave the bytes back into uint16_t vectors. This is the reverse of the shuffle operation.
            const auto d_u16_half = hn::Half<decltype(du16)>();
            auto v_interleaved_lo = hn::ZipLower(d_u16_half, v_b0, v_b1);
            auto v_interleaved_hi = hn::ZipUpper(d_u16_half, v_b0, v_b1);
            auto v_interleaved_u16 = hn::Combine(du16, v_interleaved_hi, v_interleaved_lo);

            // Continue with the rest of the pipeline
            auto v_delta_f16 = hn::BitCast(d16, v_interleaved_u16);

            VF32 v_delta_f32_lo = hn::PromoteLowerTo(d32, v_delta_f16);
            VF32 v_delta_f32_hi = hn::PromoteUpperTo(d32, v_delta_f16);

            VF32 v_prev_lo = hn::LoadU(d32, prev_snapshot_ptr + i);
            VF32 v_prev_hi = hn::LoadU(d32, prev_snapshot_ptr + i + f32_lanes);

            VF32 v_recon_lo = hn::BitCast(d32, hn::Xor(hn::BitCast(du32, v_delta_f32_lo), hn::BitCast(du32, v_prev_lo)));
            VF32 v_recon_hi = hn::BitCast(d32, hn::Xor(hn::BitCast(du32, v_delta_f32_hi), hn::BitCast(du32, v_prev_hi)));

            hn::StoreU(v_recon_lo, d32, current_out_ptr + i);
            hn::StoreU(v_recon_hi, d32, current_out_ptr + i + f32_lanes);
        }
        // SIMD PATH END
#endif
        prev_snapshot_ptr = current_out_ptr;
    }
    std::copy_n(prev_snapshot_ptr, num_floats_per_snapshot, last_snapshot_state.data());
}

} // namespace cryptodd::HWY_NAMESPACE
}
HWY_AFTER_NAMESPACE();


// =================================================================================
// PUBLIC API IMPLEMENTATION (included only once)
// =================================================================================
#if HWY_ONCE

// =================================================================================
// HIGHWAY SETUP for self-inclusion
// =================================================================================

namespace cryptodd {

HWY_EXPORT(TemporalXor);
HWY_EXPORT(DemoteFloat32ToFloat16);
HWY_EXPORT(ShuffleFloat16);
HWY_EXPORT(UnshuffleAndReconstruct);

std::vector<uint8_t> OkxObSimdCodec::encode(std::span<const float> snapshots, const OkxSnapshot& prev_snapshot) {
    const size_t num_floats = snapshots.size();
    if (num_floats % OKX_OB_SNAPSHOT_FLOATS != 0) {
        throw std::runtime_error("Snapshot data size is not a multiple of OKX_OB_SNAPSHOT_FLOATS.");
    }
    const size_t num_snapshots = num_floats / OKX_OB_SNAPSHOT_FLOATS;
    if (num_snapshots == 0) return {};

    auto xor_f32_buffer = hwy::AllocateAligned<float>(num_floats);
    auto f16_buffer = hwy::AllocateAligned<hwy::float16_t>(num_floats);
    auto shuffled_f16_buffer = hwy::AllocateAligned<uint8_t>(num_floats * sizeof(hwy::float16_t));

    HWY_DYNAMIC_DISPATCH(TemporalXor)(snapshots.data(), prev_snapshot.data(), xor_f32_buffer.get(), OKX_OB_SNAPSHOT_FLOATS);
    for (size_t s = 1; s < num_snapshots; ++s) {
        const float* current_snap = snapshots.data() + s * OKX_OB_SNAPSHOT_FLOATS;
        const float* prev_snap = snapshots.data() + (s - 1) * OKX_OB_SNAPSHOT_FLOATS;
        float* out_snap = xor_f32_buffer.get() + s * OKX_OB_SNAPSHOT_FLOATS;
        HWY_DYNAMIC_DISPATCH(TemporalXor)(current_snap, prev_snap, out_snap, OKX_OB_SNAPSHOT_FLOATS);
    }

    HWY_DYNAMIC_DISPATCH(DemoteFloat32ToFloat16)(xor_f32_buffer.get(), num_floats, f16_buffer.get());
    HWY_DYNAMIC_DISPATCH(ShuffleFloat16)(f16_buffer.get(), shuffled_f16_buffer.get(), num_floats);

    const size_t f16_bytes = num_floats * sizeof(hwy::float16_t);
    const size_t compressed_bound = ZSTD_compressBound(f16_bytes);
    std::vector<uint8_t> compressed_data(compressed_bound);
    const size_t compressed_size = ZSTD_compress(compressed_data.data(), compressed_data.size(), shuffled_f16_buffer.get(), f16_bytes, 1);

    if (ZSTD_isError(compressed_size)) {
        throw std::runtime_error(std::string("ZSTD compression failed: ") + ZSTD_getErrorName(compressed_size));
    }
    compressed_data.resize(compressed_size);
    return compressed_data;
}

std::vector<float> OkxObSimdCodec::decode(std::span<const uint8_t> encoded_data, size_t num_snapshots, OkxSnapshot& prev_snapshot) {
    if (num_snapshots == 0) return {};
    const size_t num_floats = num_snapshots * OKX_OB_SNAPSHOT_FLOATS;
    const size_t f16_bytes = num_floats * sizeof(hwy::float16_t);

    auto shuffled_f16_buffer = hwy::AllocateAligned<uint8_t>(f16_bytes);
    std::vector<float> final_output(num_floats);

    const size_t decompressed_size = ZSTD_decompress(shuffled_f16_buffer.get(), f16_bytes, encoded_data.data(), encoded_data.size());

    if (ZSTD_isError(decompressed_size)) {
        throw std::runtime_error(std::string("ZSTD decompression failed: ") + ZSTD_getErrorName(decompressed_size));
    }
    if (decompressed_size != f16_bytes) {
        throw std::runtime_error("Decompressed data size does not match expected float16 size.");
    }

    HWY_DYNAMIC_DISPATCH(UnshuffleAndReconstruct)(shuffled_f16_buffer.get(), final_output.data(), num_snapshots, prev_snapshot);

    return final_output;
}

} // namespace cryptodd
#endif // HWY_ONCE