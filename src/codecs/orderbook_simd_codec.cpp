#include <stdexcept>
#include <string>
#include <numeric>
#include <array>
#include <algorithm>
#include <vector>
#include <span>

// Bring in the class definition.
#include "orderbook_simd_codec.h"

// These headers are needed for the SIMD implementations below
#include "hwy/aligned_allocator.h"

#ifndef HWY_TARGET_INCLUDE
// Important: never ever include here ! #include <hwy/highway.h>
#ifdef HWY_HIGHWAY_INCLUDED
static_assert(false, "highway.h is already included !");
#endif
#endif

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "orderbook_simd_codec.cpp" // This file includes itself
#include "hwy/foreach_target.h"

// Must come after foreach_target.h to avoid redefinition errors.
#include "hwy/highway.h"
#include "hwy/cache_control.h"

// =================================================================================
// FORWARD DECLARATIONS for Highway's dynamic dispatch
// =================================================================================
namespace cryptodd::HWY_NAMESPACE {
    // We pass SnapshotFloats as an argument now instead of using a global constexpr
    void UnshuffleAndReconstruct16(const uint8_t* HWY_RESTRICT shuffled_in, float* HWY_RESTRICT out,
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
HWY_NOINLINE void DemoteAndXor(const float* HWY_RESTRICT current, const float* HWY_RESTRICT prev,
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

HWY_NOINLINE void DemoteFloat32ToFloat16(const float* HWY_RESTRICT in, size_t num_floats, hwy::float16_t* HWY_RESTRICT out) {
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


HWY_NOINLINE void ShuffleFloat16(const hwy::float16_t* HWY_RESTRICT in, uint8_t* HWY_RESTRICT out, size_t num_f16) {

    uint8_t* HWY_RESTRICT out_b0 = out;
    uint8_t* HWY_RESTRICT out_b1 = out + num_f16;
    size_t i = 0;

#if HWY_TARGET != HWY_SCALAR
    const hn::FixedTag<hwy::float16_t, 8> d16_128;
    const auto du16_128 = hn::Repartition<uint16_t, decltype(d16_128)>();
    const auto du8_128  = hn::Repartition<uint8_t, decltype(d16_128)>();
    const hn::FixedTag<uint8_t, 8> du8_64;

    for (; i + 8 <= num_f16; i += 8) {
        const auto v_u16 = hn::BitCast(du16_128, hn::LoadU(d16_128, in + i));

        // De-interleave the 16-bit words into two 8-bit byte planes.
        const auto lo_bytes = hn::OrderedTruncate2To(du8_128, v_u16, v_u16);
        const auto hi_bytes = hn::OrderedTruncate2To(du8_128, hn::ShiftRight<8>(v_u16), hn::ShiftRight<8>(v_u16));

        // Store 8 bytes to each plane.
        hn::StoreU(hn::ResizeBitCast(du8_64, lo_bytes), du8_64, out_b0 + i);
        hn::StoreU(hn::ResizeBitCast(du8_64, hi_bytes), du8_64, out_b1 + i);
    }
#endif

    // Scalar remainder loop
    for (; i < num_f16; ++i) {
        const auto val = hwy::BitCastScalar<uint16_t>(in[i]);
        out_b0[i] = static_cast<uint8_t>(val & 0xFF);
        out_b1[i] = static_cast<uint8_t>(val >> 8);
    }
}

// --- ENCODING PIPELINE (FLOAT32) ---

// XOR the bit patterns of two float32 vectors. No demotion needed.
HWY_NOINLINE void XorFloat32(const float* HWY_RESTRICT current, const float* HWY_RESTRICT prev,
                float* HWY_RESTRICT out, size_t num_floats)
{
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

HWY_NOINLINE void ShuffleFloat32(const float* HWY_RESTRICT in, uint8_t* HWY_RESTRICT out, size_t num_f32) {
    uint8_t* HWY_RESTRICT out_b0 = out;
    uint8_t* HWY_RESTRICT out_b1 = out + num_f32;
    uint8_t* HWY_RESTRICT out_b2 = out + 2 * num_f32;
    uint8_t* HWY_RESTRICT out_b3 = out + 3 * num_f32;
    size_t i = 0;

#if HWY_TARGET != HWY_SCALAR
    const hn::FixedTag<float, 4> d32_128;
    const auto du32_128 = hn::Repartition<uint32_t, decltype(d32_128)>();
    const auto du16_128 = hn::Repartition<uint16_t, decltype(d32_128)>();
    const auto du8_128  = hn::Repartition<uint8_t, decltype(d32_128)>();
    const hn::FixedTag<uint8_t, 4> du8_32; // For storing 4-byte chunks

    for (; i + 4 <= num_f32; i += 4) {
        const auto v_in = hn::LoadU(d32_128, in + i);

        // De-interleave floats into byte planes using two stages of Zip operations.
        // This is the inverse of the logic in UnshuffleAndReconstructFloat32.

        // Stage 1: De-interleave u32 -> u16 (separates high/low 16 bits of each float)
        const auto v_u32 = hn::BitCast(du32_128, v_in);
        const auto w_lo = hn::OrderedTruncate2To(du16_128, v_u32, v_u32);
        const auto w_hi = hn::ShiftRight<16>(v_u32);
        const auto w_hi_trunc = hn::OrderedTruncate2To(du16_128, w_hi, w_hi);

        // Stage 2: De-interleave u16 -> u8 (separates bytes)
        const auto b0 = hn::OrderedTruncate2To(du8_128, w_lo, w_lo);
        const auto b1 = hn::OrderedTruncate2To(du8_128, hn::ShiftRight<8>(w_lo), hn::ShiftRight<8>(w_lo));
        const auto b2 = hn::OrderedTruncate2To(du8_128, w_hi_trunc, w_hi_trunc);
        const auto b3 = hn::OrderedTruncate2To(du8_128, hn::ShiftRight<8>(w_hi_trunc), hn::ShiftRight<8>(w_hi_trunc));

        // Store 4 bytes to each plane.
        hn::StoreU(hn::ResizeBitCast(du8_32, b0), du8_32, out_b0 + i);
        hn::StoreU(hn::ResizeBitCast(du8_32, b1), du8_32, out_b1 + i);
        hn::StoreU(hn::ResizeBitCast(du8_32, b2), du8_32, out_b2 + i);
        hn::StoreU(hn::ResizeBitCast(du8_32, b3), du8_32, out_b3 + i);
    }
#endif

    // Scalar remainder for any leftover floats
    for (; i < num_f32; ++i) {
        const uint32_t val = hwy::BitCastScalar<uint32_t>(in[i]);
        out_b0[i] = static_cast<uint8_t>((val      ) & 0xFF);
        out_b1[i] = static_cast<uint8_t>((val >> 8 ) & 0xFF);
        out_b2[i] = static_cast<uint8_t>((val >> 16) & 0xFF);
        out_b3[i] = static_cast<uint8_t>((val >> 24) & 0xFF);
    }
}


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
        size_t i = 0;

#if HWY_TARGET != HWY_SCALAR
        const hn::FixedTag<float, 4> d32_128;
        const auto du32_128 = hn::Repartition<uint32_t, decltype(d32_128)>();
        const auto du16_128 = hn::Repartition<uint16_t, decltype(d32_128)>();
        const auto du8_128  = hn::Repartition<uint8_t, decltype(d32_128)>();
        const hn::FixedTag<uint8_t, 4> du8_32;

        for (; i + 4 <= num_floats_per_snapshot; i += 4) {
            // Load 4 bytes from each plane into small vectors.
            const auto v_b0 = hn::LoadU(du8_32, in_b0 + i);
            const auto v_b1 = hn::LoadU(du8_32, in_b1 + i);
            const auto v_b2 = hn::LoadU(du8_32, in_b2 + i);
            const auto v_b3 = hn::LoadU(du8_32, in_b3 + i);

            // **FIXED**: Use ResizeBitCast to expand 4-byte vectors to 16-byte vectors.
            // This is the correct and simple way to prepare for the Zip operations.
            const auto v_p0 = hn::ResizeBitCast(du8_128, v_b0);
            const auto v_p1 = hn::ResizeBitCast(du8_128, v_b1);
            const auto v_p2 = hn::ResizeBitCast(du8_128, v_b2);
            const auto v_p3 = hn::ResizeBitCast(du8_128, v_b3);

            // Stage 1: Interleave pairs of byte vectors into word vectors.
            const auto t0 = hn::ZipLower(du16_128, v_p0, v_p1); // [b1_0:b0_0, b1_1:b0_1, ...]
            const auto t1 = hn::ZipLower(du16_128, v_p2, v_p3); // [b3_0:b2_0, b3_1:b2_1, ...]

            // Stage 2: Interleave word vectors into dword vectors.
            const auto v_delta_u32 = hn::ZipLower(du32_128, t0, t1); // [b3_0:b2_0:b1_0:b0_0, ...]

            // Load previous state, XOR to reconstruct, and store results.
            const auto v_prev_f32 = hn::Load(d32_128, prev_snapshot_ptr + i);
            const auto v_prev_u32 = hn::BitCast(du32_128, v_prev_f32);
            const auto v_recon_u32 = hn::Xor(v_delta_u32, v_prev_u32);

            const auto v_recon_f32 = hn::BitCast(d32_128, v_recon_u32);
            hn::Store(v_recon_f32, d32_128, prev_snapshot_ptr + i);
            hn::StoreU(v_recon_f32, d32_128, current_out_ptr + i);
        }
#endif

        // Scalar remainder for any leftover floats
        for (; i < num_floats_per_snapshot; ++i) {
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
    }
    std::copy_n(prev_snapshot_ptr, num_floats_per_snapshot, last_snapshot_state.data());
}

// --- DECODING PIPELINE ---

void UnshuffleAndReconstruct16(const uint8_t* HWY_RESTRICT shuffled_in, float* HWY_RESTRICT out,
                             size_t num_snapshots, size_t snapshot_floats,
                             std::span<float> last_snapshot_state) {
    const size_t num_floats_per_snapshot = snapshot_floats;
    const size_t total_floats = num_snapshots * num_floats_per_snapshot;

    auto prev_snapshot_f16 = hwy::AllocateAligned<hwy::float16_t>(num_floats_per_snapshot);
    DemoteFloat32ToFloat16(last_snapshot_state.data(), num_floats_per_snapshot, prev_snapshot_f16.get());
    hwy::float16_t* HWY_RESTRICT prev_snapshot_ptr = prev_snapshot_f16.get();

    for (size_t s = 0; s < num_snapshots; ++s) {
        float* HWY_RESTRICT current_out_ptr = out + s * num_floats_per_snapshot;
        const size_t base_idx_bytes = s * num_floats_per_snapshot;

        size_t i = 0;

#if HWY_TARGET != HWY_SCALAR
        const hn::FixedTag<hwy::float16_t, 8> d16_128;
        const auto du16_128 = hn::Repartition<uint16_t, decltype(d16_128)>();
        const auto du8_128  = hn::Repartition<uint8_t, decltype(d16_128)>();
        const hn::FixedTag<uint8_t, 8> du8_64;
        const hn::FixedTag<float, 4> d32_128;

        for (; i + 8 <= num_floats_per_snapshot; i += 8) {
            const auto v_b0_64 = hn::LoadU(du8_64, shuffled_in + base_idx_bytes + i);
            const auto v_b1_64 = hn::LoadU(du8_64, shuffled_in + total_floats + base_idx_bytes + i);

            const auto v_b0_128 = hn::ResizeBitCast(du8_128, v_b0_64);
            const auto v_b1_128 = hn::ResizeBitCast(du8_128, v_b1_64);

            const auto v_delta_u16 = hn::ZipLower(du16_128, v_b0_128, v_b1_128);

            const auto v_prev_u16 = hn::BitCast(du16_128, hn::LoadU(d16_128, prev_snapshot_ptr + i));
            const auto v_recon_u16 = hn::Xor(v_delta_u16, v_prev_u16);
            const auto v_recon_f16 = hn::BitCast(d16_128, v_recon_u16);
            hn::StoreU(v_recon_f16, d16_128, prev_snapshot_ptr + i);

            auto v_out_f32_lo = hn::PromoteLowerTo(d32_128, v_recon_f16);
            auto v_out_f32_hi = hn::PromoteUpperTo(d32_128, v_recon_f16);
            hn::StoreU(v_out_f32_lo, d32_128, current_out_ptr + i);
            hn::StoreU(v_out_f32_hi, d32_128, current_out_ptr + i + 4);
        }
#endif

        // Scalar remainder loop
        for (; i < num_floats_per_snapshot; ++i) {
            const uint8_t b0 = shuffled_in[base_idx_bytes + i];
            const uint8_t b1 = shuffled_in[total_floats + base_idx_bytes + i];
            const uint16_t u16_delta = (static_cast<uint16_t>(b1) << 8) | b0;
            const uint16_t u16_prev = hwy::BitCastScalar<uint16_t>(prev_snapshot_ptr[i]);
            const uint16_t u16_recon = u16_delta ^ u16_prev;
            prev_snapshot_ptr[i] = hwy::BitCastScalar<hwy::float16_t>(u16_recon);
            current_out_ptr[i] = hwy::ConvertScalarTo<float>(prev_snapshot_ptr[i]);
        }
    }

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
HWY_NOINLINE void simd::DemoteAndXor_dispatcher(const float* current, const float* prev, hwy::float16_t* out, size_t num_floats) {
    HWY_DYNAMIC_DISPATCH(DemoteAndXor)(current, prev, out, num_floats);
}

HWY_EXPORT(ShuffleFloat16);
HWY_NOINLINE void simd::ShuffleFloat16_dispatcher(const hwy::float16_t* in, uint8_t* out, size_t num_f16) {
    HWY_DYNAMIC_DISPATCH(ShuffleFloat16)(in, out, num_f16);
}

HWY_EXPORT(UnshuffleAndReconstruct16);
HWY_NOINLINE void simd::UnshuffleAndReconstruct_dispatcher(const uint8_t* shuffled_in, float* out, size_t num_snapshots, size_t snapshot_floats, std::span<float> last_snapshot_state) {
    HWY_DYNAMIC_DISPATCH(UnshuffleAndReconstruct16)(shuffled_in, out, num_snapshots, snapshot_floats, last_snapshot_state);
}

HWY_EXPORT(XorFloat32);
HWY_NOINLINE void simd::XorFloat32_dispatcher(const float* current, const float* prev, float* out, size_t num_floats) {
    HWY_DYNAMIC_DISPATCH(XorFloat32)(current, prev, out, num_floats);
}

HWY_EXPORT(ShuffleFloat32);
HWY_NOINLINE void simd::ShuffleFloat32_dispatcher(const float* in, uint8_t* out, size_t num_f32) {
    HWY_DYNAMIC_DISPATCH(ShuffleFloat32)(in, out, num_f32);
}

HWY_EXPORT(UnshuffleAndReconstructFloat32);
HWY_NOINLINE void simd::UnshuffleAndReconstructFloat32_dispatcher(const uint8_t* shuffled_in, float* out, size_t num_snapshots, size_t snapshot_floats, std::span<float> last_snapshot_state) {
    HWY_DYNAMIC_DISPATCH(UnshuffleAndReconstructFloat32)(shuffled_in, out, num_snapshots, snapshot_floats, last_snapshot_state);
}

} // namespace cryptodd
#endif // HWY_ONCE
