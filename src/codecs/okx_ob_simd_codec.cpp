#include <stdexcept>
#include <string>
#include <numeric>
#include <array>
#include <algorithm>
#include <vector>
#include <span>

// Bring in the class definition.
#include "okx_ob_simd_codec.h"

// These headers are needed for the SIMD implementations below
#include "hwy/aligned_allocator.h"

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "okx_ob_simd_codec.cpp" // This file includes itself
#include "hwy/foreach_target.h"

// Must come after foreach_target.h to avoid redefinition errors.
#include "hwy/highway.h"

// =================================================================================
// FORWARD DECLARATIONS for Highway's dynamic dispatch
// =================================================================================
namespace cryptodd::HWY_NAMESPACE {
    // We pass SnapshotFloats as an argument now instead of using a global constexpr
    void UnshuffleAndReconstruct(const uint8_t* HWY_RESTRICT shuffled_in, float* HWY_RESTRICT out,
                                 size_t num_snapshots, size_t snapshot_floats,
                                 std::span<float> last_snapshot_state);

    void DemoteFloat32ToFloat16(const float* HWY_RESTRICT in, size_t num_floats, hwy::float16_t* HWY_RESTRICT out);

    void XorFloat32(const float* HWY_RESTRICT current, const float* HWY_RESTRICT prev,
                    float* HWY_RESTRICT out, size_t num_floats);

    void ShuffleFloat32(const float* HWY_RESTRICT in, uint8_t* HWY_RESTRICT out, size_t num_f32);

    void UnshuffleAndReconstructFloat32(const uint8_t* HWY_RESTRICT shuffled_in, float* HWY_RESTRICT out,
                                        size_t num_snapshots, size_t snapshot_floats,
                                        std::span<float> last_snapshot_state);
}

// =================================================================================
// SIMD IMPLEMENTATIONS (included multiple times by foreach_target.h)
// =================================================================================

HWY_BEFORE_NAMESPACE();
namespace cryptodd
{
namespace HWY_NAMESPACE{

// Highway namespace aliases
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

// New combined function: Demote float32 to float16, then XOR their bit patterns.
void DemoteAndXor(const float* HWY_RESTRICT current, const float* HWY_RESTRICT prev,
                  hwy::float16_t* HWY_RESTRICT out, size_t num_floats) {
    size_t i = 0;

#if HWY_TARGET != HWY_SCALAR
    // SIMD PATH
    const hn::Half<decltype(d16)> d16_half;
    const size_t f32_lanes = hn::Lanes(d32);
    const size_t f16_lanes = hn::Lanes(d16);

    for (; i + f16_lanes <= num_floats; i += f16_lanes) {
        // Load two vectors of f32 for each input (current and previous)
        const VF32 v_curr_f32_lo = hn::LoadU(d32, current + i);
        const VF32 v_curr_f32_hi = hn::LoadU(d32, current + i + f32_lanes);
        const VF32 v_prev_f32_lo = hn::LoadU(d32, prev + i);
        const VF32 v_prev_f32_hi = hn::LoadU(d32, prev + i + f32_lanes);

        // --- DEMOTE FIRST ---
        auto v_curr_f16_lo = hn::DemoteTo(d16_half, v_curr_f32_lo);
        auto v_curr_f16_hi = hn::DemoteTo(d16_half, v_curr_f32_hi);
        auto v_prev_f16_lo = hn::DemoteTo(d16_half, v_prev_f32_lo);
        auto v_prev_f16_hi = hn::DemoteTo(d16_half, v_prev_f32_hi);

        // Combine halves into full f16 vectors
        VF16 v_curr_f16 = hn::Combine(d16, v_curr_f16_hi, v_curr_f16_lo);
        VF16 v_prev_f16 = hn::Combine(d16, v_prev_f16_hi, v_prev_f16_lo);

        // --- THEN XOR ---
        // BitCast to integer type for XOR
        VU16 v_curr_u16 = hn::BitCast(du16, v_curr_f16);
        VU16 v_prev_u16 = hn::BitCast(du16, v_prev_f16);
        VU16 v_xor_u16 = hn::Xor(v_curr_u16, v_prev_u16);

        // Store the result (which is a float16 bit pattern)
        hn::StoreU(hn::BitCast(d16, v_xor_u16), d16, out + i);
    }
#endif

    // SCALAR/REMAINDER PATH
    for (; i < num_floats; ++i) {
        // --- DEMOTE FIRST ---
        const hwy::float16_t f16_curr = hwy::ConvertScalarTo<hwy::float16_t>(current[i]);
        const hwy::float16_t f16_prev = hwy::ConvertScalarTo<hwy::float16_t>(prev[i]);
        // --- THEN XOR ---
        const uint16_t u16_xor = hwy::BitCastScalar<uint16_t>(f16_curr) ^ hwy::BitCastScalar<uint16_t>(f16_prev);
        out[i] = hwy::BitCastScalar<hwy::float16_t>(u16_xor);
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
        const auto val = hwy::BitCastScalar<uint16_t>(in[i]);
        out_b0[i] = static_cast<uint8_t>(val & 0xFF);
        out_b1[i] = static_cast<uint8_t>(val >> 8);
    }
    // SIMD PATH END
#endif
}

// --- ENCODING PIPELINE (FLOAT32) ---

// XOR the bit patterns of two float32 vectors. No demotion needed.
void XorFloat32(const float* HWY_RESTRICT current, const float* HWY_RESTRICT prev,
                float* HWY_RESTRICT out, size_t num_floats) {
    size_t i = 0;

#if HWY_TARGET != HWY_SCALAR
    // SIMD PATH
    const size_t f32_lanes = hn::Lanes(d32);
    for (; i + f32_lanes <= num_floats; i += f32_lanes) {
        const VF32 v_curr_f32 = hn::LoadU(d32, current + i);
        const VF32 v_prev_f32 = hn::LoadU(d32, prev + i);

        // BitCast to integer type for XOR
        const VU32 v_curr_u32 = hn::BitCast(du32, v_curr_f32);
        const VU32 v_prev_u32 = hn::BitCast(du32, v_prev_f32);
        const VU32 v_xor_u32 = hn::Xor(v_curr_u32, v_prev_u32);

        // Store the result (which is a float32 bit pattern)
        hn::StoreU(hn::BitCast(d32, v_xor_u32), d32, out + i);
    }
#endif

    // SCALAR/REMAINDER PATH
    for (; i < num_floats; ++i) {
        const uint32_t u32_curr = hwy::BitCastScalar<uint32_t>(current[i]);
        const uint32_t u32_prev = hwy::BitCastScalar<uint32_t>(prev[i]);
        const uint32_t u32_xor = u32_curr ^ u32_prev;
        out[i] = hwy::BitCastScalar<float>(u32_xor);
    }
}

// De-interleaves float32 values (viewed as uint32_t) into four separate
// planes for each of the four bytes.
void ShuffleFloat32(const float* HWY_RESTRICT in, uint8_t* HWY_RESTRICT out, size_t num_f32) {
    uint8_t* HWY_RESTRICT out_b0 = out;
    uint8_t* HWY_RESTRICT out_b1 = out + num_f32;
    uint8_t* HWY_RESTRICT out_b2 = out + 2 * num_f32;
    uint8_t* HWY_RESTRICT out_b3 = out + 3 * num_f32;

#if HWY_TARGET == HWY_SCALAR
    // SCALAR PATH
    for (size_t i = 0; i < num_f32; ++i) {
        const uint32_t val = hwy::BitCastScalar<uint32_t>(in[i]);
        out_b0[i] = static_cast<uint8_t>((val      ) & 0xFF);
        out_b1[i] = static_cast<uint8_t>((val >> 8 ) & 0xFF);
        out_b2[i] = static_cast<uint8_t>((val >> 16) & 0xFF);
        out_b3[i] = static_cast<uint8_t>((val >> 24) & 0xFF);
    }
#else
    // SIMD PATH
    const hn::Repartition<uint16_t, decltype(du32)> d_u16_from_u32;
    const hn::Repartition<uint8_t, decltype(du16)> d_u8_from_u16;

    const size_t lanes_u32 = hn::Lanes(du32);
    // Process two u32 vectors per loop to feed OrderedTruncate2To
    const size_t step = 2 * lanes_u32;
    size_t i = 0;

    for (; i + step <= num_f32; i += step) {
        // Load two vectors of float32 and view as uint32
        const auto v32_a = hn::BitCast(du32, hn::LoadU(d32, in + i));
        const auto v32_b = hn::BitCast(du32, hn::LoadU(d32, in + i + lanes_u32));

        // --- STAGE 1: De-interleave u32 vectors into u16 parts ---
        const VU32 mask16 = hn::Set(du32, 0x0000FFFF);
        const auto lo16_parts_a = hn::And(v32_a, mask16);
        const auto hi16_parts_a = hn::ShiftRight<16>(v32_a);
        const auto lo16_parts_b = hn::And(v32_b, mask16);
        const auto hi16_parts_b = hn::ShiftRight<16>(v32_b);

        // Pack the parts into two u16 vectors: one for bytes {0,1} and one for {2,3}
        const auto v16_plane_01 = hn::OrderedTruncate2To(d_u16_from_u32, lo16_parts_a, lo16_parts_b);
        const auto v16_plane_23 = hn::OrderedTruncate2To(d_u16_from_u32, hi16_parts_a, hi16_parts_b);

        // --- STAGE 2: De-interleave each u16 plane into u8 parts ---
        const VU16 mask8 = hn::Set(du16, 0x00FF);
        const auto b0_parts = hn::And(v16_plane_01, mask8);
        const auto b1_parts = hn::ShiftRight<8>(v16_plane_01);
        const auto b2_parts = hn::And(v16_plane_23, mask8);
        const auto b3_parts = hn::ShiftRight<8>(v16_plane_23);

        // Pack the final byte planes
        auto plane0 = hn::OrderedTruncate2To(d_u8_from_u16, b0_parts, b0_parts);
        auto plane1 = hn::OrderedTruncate2To(d_u8_from_u16, b1_parts, b1_parts);
        auto plane2 = hn::OrderedTruncate2To(d_u8_from_u16, b2_parts, b2_parts);
        auto plane3 = hn::OrderedTruncate2To(d_u8_from_u16, b3_parts, b3_parts);

        // Store the byte planes
        hn::StoreU(plane0, d_u8_from_u16, out_b0 + i);
        hn::StoreU(plane1, d_u8_from_u16, out_b1 + i);
        hn::StoreU(plane2, d_u8_from_u16, out_b2 + i);
        hn::StoreU(plane3, d_u8_from_u16, out_b3 + i);
    }

    // REMAINDER PATH
    for (; i < num_f32; ++i) {
        const uint32_t val = hwy::BitCastScalar<uint32_t>(in[i]);
        out_b0[i] = static_cast<uint8_t>((val      ) & 0xFF);
        out_b1[i] = static_cast<uint8_t>((val >> 8 ) & 0xFF);
        out_b2[i] = static_cast<uint8_t>((val >> 16) & 0xFF);
        out_b3[i] = static_cast<uint8_t>((val >> 24) & 0xFF);
    }
#endif
}

// --- DECODING PIPELINE (FLOAT32) ---

// --- DECODING PIPELINE (FLOAT32) ---
void UnshuffleAndReconstructFloat32(const uint8_t* HWY_RESTRICT shuffled_in, float* HWY_RESTRICT out,
                                    size_t num_snapshots, size_t snapshot_floats,
                                    std::span<float> last_snapshot_state) {
    const size_t num_floats_per_snapshot = snapshot_floats;
    const size_t total_floats = num_snapshots * num_floats_per_snapshot;

    auto prev_snapshot_f32 = hwy::AllocateAligned<float>(num_floats_per_snapshot);
    std::copy_n(last_snapshot_state.data(), num_floats_per_snapshot, prev_snapshot_f32.get());
    float* HWY_RESTRICT prev_snapshot_ptr = prev_snapshot_f32.get();

    for (size_t s = 0; s < num_snapshots; ++s) {
        float* HWY_RESTRICT current_out_ptr = out + s * num_floats_per_snapshot;
        const size_t base_idx = s * num_floats_per_snapshot;
        const uint8_t* HWY_RESTRICT in_b0 = shuffled_in + base_idx;
        const uint8_t* HWY_RESTRICT in_b1 = shuffled_in + total_floats + base_idx;
        const uint8_t* HWY_RESTRICT in_b2 = shuffled_in + 2 * total_floats + base_idx;
        const uint8_t* HWY_RESTRICT in_b3 = shuffled_in + 3 * total_floats + base_idx;

#if HWY_TARGET == HWY_SCALAR
        // SCALAR PATH
        for (size_t i = 0; i < num_floats_per_snapshot; ++i) {
            const uint32_t u32_delta = (static_cast<uint32_t>(in_b3[i]) << 24) |
                                       (static_cast<uint32_t>(in_b2[i]) << 16) |
                                       (static_cast<uint32_t>(in_b1[i]) << 8)  |
                                       in_b0[i];
            const uint32_t u32_prev = hwy::BitCastScalar<uint32_t>(prev_snapshot_ptr[i]);
            const uint32_t u32_recon = u32_delta ^ u32_prev;
            const float f32_recon = hwy::BitCastScalar<float>(u32_recon);
            prev_snapshot_ptr[i] = f32_recon;
            current_out_ptr[i] = f32_recon;
        }
#else
        // SIMD PATH
        const hn::Repartition<uint8_t, decltype(du16)> d_u8_from_u16;
        const hn::Repartition<uint16_t, decltype(d_u8_from_u16)> d_u16_from_u8;
        const hn::Repartition<uint32_t, decltype(d_u16_from_u8)> d_u32_from_u16;

        const size_t lanes_u32 = hn::Lanes(d32);
        // Process two u32 vectors per loop to match the shuffle logic
        const size_t step = 2 * lanes_u32;

        for (size_t i = 0; i < num_floats_per_snapshot; i += step) {
            // Load bytes from all four planes. Each load gets enough bytes for `step` floats.
            const auto vb0 = hn::LoadU(d_u8_from_u16, in_b0 + i);
            const auto vb1 = hn::LoadU(d_u8_from_u16, in_b1 + i);
            const auto vb2 = hn::LoadU(d_u8_from_u16, in_b2 + i);
            const auto vb3 = hn::LoadU(d_u8_from_u16, in_b3 + i);

            // STAGE 1: Re-interleave u8 planes into u16 planes
            // Note: The descriptor for ZipLower/Upper is for the OUTPUT type (u16).
            const auto v16_01 = hn::ZipLower(d_u16_from_u8, vb0, vb1);
            const auto v16_23 = hn::ZipLower(d_u16_from_u8, vb2, vb3);
            const auto v16_01_hi = hn::ZipUpper(d_u16_from_u8, vb0, vb1);
            const auto v16_23_hi = hn::ZipUpper(d_u16_from_u8, vb2, vb3);

            // STAGE 2: Re-interleave u16 planes into u32 delta vectors
            const VU32 v_delta_u32_a = hn::ZipLower(d_u32_from_u16, v16_01, v16_23);
            const VU32 v_delta_u32_b = hn::ZipUpper(d_u32_from_u16, v16_01_hi, v16_23_hi);

            // Reconstruct the first vector
            const VF32 v_prev_f32_a = hn::LoadU(d32, prev_snapshot_ptr + i);
            const VU32 v_recon_u32_a = hn::Xor(v_delta_u32_a, hn::BitCast(du32, v_prev_f32_a));
            const VF32 v_recon_f32_a = hn::BitCast(d32, v_recon_u32_a);
            hn::StoreU(v_recon_f32_a, d32, prev_snapshot_ptr + i);
            hn::StoreU(v_recon_f32_a, d32, current_out_ptr + i);

            // Reconstruct the second vector
            const VF32 v_prev_f32_b = hn::LoadU(d32, prev_snapshot_ptr + i + lanes_u32);
            const VU32 v_recon_u32_b = hn::Xor(v_delta_u32_b, hn::BitCast(du32, v_prev_f32_b));
            const VF32 v_recon_f32_b = hn::BitCast(d32, v_recon_u32_b);
            hn::StoreU(v_recon_f32_b, d32, prev_snapshot_ptr + i + lanes_u32);
            hn::StoreU(v_recon_f32_b, d32, current_out_ptr + i + lanes_u32);
        }
#endif
    }
    std::copy_n(prev_snapshot_ptr, num_floats_per_snapshot, last_snapshot_state.data());
}


// --- DECODING PIPELINE ---

void UnshuffleAndReconstruct(const uint8_t* HWY_RESTRICT shuffled_in, float* HWY_RESTRICT out,
                             size_t num_snapshots, size_t snapshot_floats,
                             std::span<float> last_snapshot_state) {
    const size_t num_floats_per_snapshot = snapshot_floats;
    const size_t total_floats = num_snapshots * num_floats_per_snapshot;

    // We need the previous state in float16 format for the reconstruction loop.
    // So we demote it once here.
    auto prev_snapshot_f16 = hwy::AllocateAligned<hwy::float16_t>(num_floats_per_snapshot);
    DemoteFloat32ToFloat16(last_snapshot_state.data(), num_floats_per_snapshot, prev_snapshot_f16.get());    hwy::float16_t* HWY_RESTRICT prev_snapshot_ptr = prev_snapshot_f16.get();

    for (size_t s = 0; s < num_snapshots; ++s) {
        float* HWY_RESTRICT current_out_ptr = out + s * num_floats_per_snapshot;
        const size_t base_idx_bytes = s * num_floats_per_snapshot;
#if HWY_TARGET == HWY_SCALAR
        // SCALAR PATH START
        for (size_t i = 0; i < num_floats_per_snapshot; ++i) {
            // Unshuffle bytes
            const uint8_t b0 = shuffled_in[base_idx_bytes + i];
            const uint8_t b1 = shuffled_in[total_floats + base_idx_bytes + i];
            const uint16_t u16_delta = (static_cast<uint16_t>(b1) << 8) | b0;

            // Get previous state as uint16
            const uint16_t u16_prev = hwy::BitCastScalar<uint16_t>(prev_snapshot_ptr[i]);
            
            // Reconstruct the uint16 bit pattern
            const uint16_t u16_recon = u16_delta ^ u16_prev;
            
            // This reconstructed uint16 IS the new state for the next snapshot
            prev_snapshot_ptr[i] = hwy::BitCastScalar<hwy::float16_t>(u16_recon);
            
            // Promote the final value to float32 for output
            current_out_ptr[i] = hwy::ConvertScalarTo<float>(prev_snapshot_ptr[i]);
        }
        // SCALAR PATH END
#else
        // SIMD PATH START
        const size_t f32_lanes = hn::Lanes(d32);
        const size_t f16_lanes = hn::Lanes(d16);
        
        // Process a full f16 vector's worth of floats at a time.
        for (size_t i = 0; i < num_floats_per_snapshot; i += f16_lanes) {
            // Unshuffle bytes into a float16 vector of DELTAs
            const hn::Rebind<uint8_t, decltype(d16)> d_u8_rebind;
            const auto v_b0 = hn::LoadU(d_u8_rebind, shuffled_in + base_idx_bytes + i);
            const auto v_b1 = hn::LoadU(d_u8_rebind, shuffled_in + total_floats + base_idx_bytes + i);
            
            // Interleave the bytes back into uint16_t vectors. This is the reverse of the shuffle operation.
            const auto d_u16_half = hn::Half<decltype(du16)>();
            auto v_interleaved_lo = hn::ZipLower(d_u16_half, v_b0, v_b1);
            auto v_interleaved_hi = hn::ZipUpper(d_u16_half, v_b0, v_b1);
            VU16 v_delta_u16 = hn::Combine(du16, v_interleaved_hi, v_interleaved_lo);

            // Load previous state (as float16)
            VF16 v_prev_f16 = hn::LoadU(d16, prev_snapshot_ptr + i);
            VU16 v_prev_u16 = hn::BitCast(du16, v_prev_f16);

            // Reconstruct the float16 bit pattern
            VU16 v_recon_u16 = hn::Xor(v_delta_u16, v_prev_u16);
            VF16 v_recon_f16 = hn::BitCast(d16, v_recon_u16);

            // This result IS the new state for the next snapshot. Store it back.
            hn::StoreU(v_recon_f16, d16, prev_snapshot_ptr + i);

            // Promote the final reconstructed float16 to float32 for output
            VF32 v_out_f32_lo = hn::PromoteLowerTo(d32, v_recon_f16);
            VF32 v_out_f32_hi = hn::PromoteUpperTo(d32, v_recon_f16);
            hn::StoreU(v_out_f32_lo, d32, current_out_ptr + i);
            hn::StoreU(v_out_f32_hi, d32, current_out_ptr + i + f32_lanes);
        }
        // SIMD PATH END
#endif
        // Here, prev_snapshot_ptr has been updated in-place to become the current snapshot's state
    }

    // Now, update the output state by promoting our final f16 state back to f32
    for(size_t i = 0; i < num_floats_per_snapshot; ++i) {
        last_snapshot_state[i] = hwy::ConvertScalarTo<float>(prev_snapshot_ptr[i]);
    }
}

} // namespace cryptodd::HWY_NAMESPACE
}
HWY_AFTER_NAMESPACE();


// =================================================================================
// PUBLIC API IMPLEMENTATION (included only once)
// =================================================================================
#if HWY_ONCE
namespace cryptodd {

// Define dispatchers that the header-based template functions can call.
HWY_EXPORT(DemoteAndXor);
void DemoteAndXor_dispatcher(const float* current, const float* prev, hwy::float16_t* out, size_t num_floats) {
    HWY_DYNAMIC_DISPATCH(DemoteAndXor)(current, prev, out, num_floats);
}

HWY_EXPORT(ShuffleFloat16);
void ShuffleFloat16_dispatcher(const hwy::float16_t* in, uint8_t* out, size_t num_f16) {
    HWY_DYNAMIC_DISPATCH(ShuffleFloat16)(in, out, num_f16);
}

HWY_EXPORT(UnshuffleAndReconstruct);
void UnshuffleAndReconstruct_dispatcher(const uint8_t* shuffled_in, float* out, size_t num_snapshots, size_t snapshot_floats, std::span<float> last_snapshot_state) {
    HWY_DYNAMIC_DISPATCH(UnshuffleAndReconstruct)(shuffled_in, out, num_snapshots, snapshot_floats, last_snapshot_state);
}

HWY_EXPORT(XorFloat32);
void XorFloat32_dispatcher(const float* current, const float* prev, float* out, size_t num_floats) {
    HWY_DYNAMIC_DISPATCH(XorFloat32)(current, prev, out, num_floats);
}

HWY_EXPORT(ShuffleFloat32);
void ShuffleFloat32_dispatcher(const float* in, uint8_t* out, size_t num_f32) {
    HWY_DYNAMIC_DISPATCH(ShuffleFloat32)(in, out, num_f32);
}

HWY_EXPORT(UnshuffleAndReconstructFloat32);
void UnshuffleAndReconstructFloat32_dispatcher(const uint8_t* shuffled_in, float* out, size_t num_snapshots, size_t snapshot_floats, std::span<float> last_snapshot_state) {
    HWY_DYNAMIC_DISPATCH(UnshuffleAndReconstructFloat32)(shuffled_in, out, num_snapshots, snapshot_floats, last_snapshot_state);
}

} // namespace cryptodd
#endif // HWY_ONCE