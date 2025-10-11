#include "temporal_2d_simd_codec.h"

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "temporal_2d_simd_codec.cpp"
#include "hwy/foreach_target.h"

#include "hwy/highway.h"
#include "hwy/cache_control.h"

// The file is included multiple times by foreach_target.h, so we need this guard
// to prevent re-defining the functions for each target.
HWY_BEFORE_NAMESPACE();
namespace cryptodd::HWY_NAMESPACE {

namespace hn = hwy::HWY_NAMESPACE;

// SIMD vector types
inline constexpr hn::ScalableTag<float> d32;
inline constexpr hn::ScalableTag<hwy::float16_t> d16;
inline constexpr hn::ScalableTag<uint32_t> du32;
inline constexpr hn::ScalableTag<uint16_t> du16;
inline constexpr hn::ScalableTag<uint8_t> du8;
inline constexpr hn::ScalableTag<int64_t> di64;
inline constexpr hn::ScalableTag<uint64_t> du64;

using VF32 = hn::Vec<decltype(d32)>;
using VU32 = hn::Vec<decltype(du32)>;
using VF16 = hn::Vec<decltype(d16)>;
using VU16 = hn::Vec<decltype(du16)>;
using VU8 = hn::Vec<decltype(du8)>;
using VI64 = hn::Vec<decltype(di64)>;
using VU64 = hn::Vec<decltype(du64)>;

HWY_NOINLINE void DemoteAndXor2D(const float* HWY_RESTRICT soa_data, const float* HWY_RESTRICT prev_row,
                                 hwy::float16_t* HWY_RESTRICT out, size_t num_rows, size_t num_features) {
    if (num_rows == 0) return;
    const hn::Half<decltype(d16)> d16_half;
    const size_t f32_lanes = hn::Lanes(d32);
    const size_t f16_lanes = hn::Lanes(d16);

    for (size_t f = 0; f < num_features; ++f) {
        const float* HWY_RESTRICT feature_col_in = soa_data + f * num_rows;
        hwy::float16_t* HWY_RESTRICT feature_col_out = out + f * num_rows;

        // First element uses prev_row
        const hwy::float16_t f16_curr = hwy::ConvertScalarTo<hwy::float16_t>(feature_col_in[0]);
        const hwy::float16_t f16_prev = hwy::ConvertScalarTo<hwy::float16_t>(prev_row[f]);
        const uint16_t u16_xor = hwy::BitCastScalar<uint16_t>(f16_curr) ^ hwy::BitCastScalar<uint16_t>(f16_prev);
        feature_col_out[0] = hwy::BitCastScalar<hwy::float16_t>(u16_xor);

        size_t i = 1;
#if HWY_TARGET != HWY_SCALAR
        for (; i + f16_lanes <= num_rows; i += f16_lanes) {
            const VF32 v_curr_f32_lo = hn::LoadU(d32, feature_col_in + i);
            const VF32 v_curr_f32_hi = hn::LoadU(d32, feature_col_in + i + f32_lanes);
            const VF32 v_prev_f32_lo = hn::LoadU(d32, feature_col_in + i - 1); // i-1 is safe due to loop start
            const VF32 v_prev_f32_hi = hn::LoadU(d32, feature_col_in + i - 1 + f32_lanes);

            auto v_curr_f16_lo = hn::DemoteTo(d16_half, v_curr_f32_lo);
            auto v_curr_f16_hi = hn::DemoteTo(d16_half, v_curr_f32_hi);
            auto v_prev_f16_lo = hn::DemoteTo(d16_half, v_prev_f32_lo);
            auto v_prev_f16_hi = hn::DemoteTo(d16_half, v_prev_f32_hi);

            VF16 v_curr_f16 = hn::Combine(d16, v_curr_f16_hi, v_curr_f16_lo);
            VF16 v_prev_f16 = hn::Combine(d16, v_prev_f16_hi, v_prev_f16_lo);

            VU16 v_curr_u16 = hn::BitCast(du16, v_curr_f16);
            VU16 v_prev_u16 = hn::BitCast(du16, v_prev_f16);
            VU16 v_xor_u16 = hn::Xor(v_curr_u16, v_prev_u16);

            hn::StoreU(hn::BitCast(d16, v_xor_u16), d16, feature_col_out + i);
        }
#endif
        // Scalar remainder loop
        for (; i < num_rows; ++i) {
            const hwy::float16_t f16_c = hwy::ConvertScalarTo<hwy::float16_t>(feature_col_in[i]);
            const hwy::float16_t f16_p = hwy::ConvertScalarTo<hwy::float16_t>(feature_col_in[i - 1]);
            const uint16_t u16_x = hwy::BitCastScalar<uint16_t>(f16_c) ^ hwy::BitCastScalar<uint16_t>(f16_p);
            feature_col_out[i] = hwy::BitCastScalar<hwy::float16_t>(u16_x);
        }
    }
}

HWY_NOINLINE void ShuffleFloat16_2D(const hwy::float16_t* HWY_RESTRICT in, uint8_t* HWY_RESTRICT out,
                                    size_t num_rows, size_t num_features) {
    const size_t total_elements = num_rows * num_features;
    const hn::Repartition<uint8_t, decltype(du16)> d_u8_packed;
    const size_t lanes_u16 = hn::Lanes(du16);
    const size_t step = 2 * lanes_u16;
    
    for (size_t f = 0; f < num_features; ++f) {
        const hwy::float16_t* HWY_RESTRICT feature_col_in = in + f * num_rows;
        uint8_t* HWY_RESTRICT out_b0 = out + f * num_rows;
        uint8_t* HWY_RESTRICT out_b1 = out + total_elements + f * num_rows;

        size_t i = 0;
#if HWY_TARGET != HWY_SCALAR
        for (; i + step <= num_rows; i += step) {
            const VU16 v_in_a = hn::BitCast(du16, hn::LoadU(d16, feature_col_in + i));
            const VU16 v_in_b = hn::BitCast(du16, hn::LoadU(d16, feature_col_in + i + lanes_u16));

            const VU16 lo_mask = hn::Set(du16, 0x00FF);
            const VU16 v_lo_a = hn::And(v_in_a, lo_mask);
            const VU16 v_hi_a = hn::ShiftRight<8>(v_in_a);
            const VU16 v_lo_b = hn::And(v_in_b, lo_mask);
            const VU16 v_hi_b = hn::ShiftRight<8>(v_in_b);

            auto packed_lo = hn::OrderedTruncate2To(d_u8_packed, v_lo_a, v_lo_b);
            auto packed_hi = hn::OrderedTruncate2To(d_u8_packed, v_hi_a, v_hi_b);

            hn::StoreU(packed_lo, d_u8_packed, out_b0 + i);
            hn::StoreU(packed_hi, d_u8_packed, out_b1 + i);
        }
#endif
        // Scalar remainder loop
        for (; i < num_rows; ++i) {
            const auto val = hwy::BitCastScalar<uint16_t>(feature_col_in[i]);
            out_b0[i] = static_cast<uint8_t>(val & 0xFF);
            out_b1[i] = static_cast<uint8_t>(val >> 8);
        }
    }
}

HWY_NOINLINE void UnshuffleAndReconstruct16_2D(const uint8_t* HWY_RESTRICT shuffled_in, float* HWY_RESTRICT out,
                                               size_t num_rows, size_t num_features, std::span<float> prev_row_state) {
    const size_t total_elements = num_rows * num_features;
    const size_t f32_lanes = hn::Lanes(d32);
    const size_t f16_lanes = hn::Lanes(d16);

    for (size_t f = 0; f < num_features; ++f) {
        float* HWY_RESTRICT feature_out = out + f * num_rows;
        const uint8_t* HWY_RESTRICT in_b0 = shuffled_in + f * num_rows;
        const uint8_t* HWY_RESTRICT in_b1 = shuffled_in + total_elements + f * num_rows;

        hwy::float16_t prev_f16 = hwy::ConvertScalarTo<hwy::float16_t>(prev_row_state[f]);

        size_t i = 0;
#if HWY_TARGET != HWY_SCALAR
        for (; i + f16_lanes <= num_rows; i += f16_lanes) {
            constexpr size_t kPrefetchLookahead = 2;
            if (i + (kPrefetchLookahead * f16_lanes) < num_rows) {
                hwy::Prefetch(in_b0 + i + (kPrefetchLookahead * f16_lanes));
                hwy::Prefetch(in_b1 + i + (kPrefetchLookahead * f16_lanes));
            }

            const hn::Rebind<uint8_t, decltype(d16)> d_u8_rebind;
            const auto v_b0 = hn::LoadU(d_u8_rebind, in_b0 + i);
            const auto v_b1 = hn::LoadU(d_u8_rebind, in_b1 + i);

            const auto d_u16_half = hn::Half<decltype(du16)>();
            auto v_interleaved_lo = hn::ZipLower(d_u16_half, v_b0, v_b1);
            auto v_interleaved_hi = hn::ZipUpper(d_u16_half, v_b0, v_b1);
            VU16 v_delta_u16 = hn::Combine(du16, v_interleaved_hi, v_interleaved_lo);

            // This vector will hold the scan result. Initialize with deltas.
            VU16 v_scan = v_delta_u16;

            // Parallel Prefix XOR Scan using logarithmic shifts.
            // Correct Parallel Inclusive Prefix XOR Scan
            for (size_t dist = 1; dist < f16_lanes; dist *= 2) {
                v_scan = hn::Xor(v_scan, hn::SlideUpLanes(du16, v_scan, dist));
            }

            // The scan is now complete relative to the start of the vector.
            // Now, chain it with the final value from the previous block (`prev_f16`).
            const VU16 v_prev_broadcast = hn::Set(du16, hwy::BitCastScalar<uint16_t>(prev_f16));
            VU16 v_recon_u16 = hn::Xor(v_scan, v_prev_broadcast);

            // Update the `prev_f16` state for the *next* SIMD block.
            prev_f16 = hwy::BitCastScalar<hwy::float16_t>(hn::ExtractLane(v_recon_u16, f16_lanes - 1));

            VF16 v_recon_f16 = hn::BitCast(d16, v_recon_u16);

            VF32 v_out_f32_lo = hn::PromoteLowerTo(d32, v_recon_f16);
            VF32 v_out_f32_hi = hn::PromoteUpperTo(d32, v_recon_f16);
            hn::StoreU(v_out_f32_lo, d32, feature_out + i);
            hn::StoreU(v_out_f32_hi, d32, feature_out + i + f32_lanes);
        }
#endif
        // Scalar remainder loop
        uint16_t prev_u16_scalar = hwy::BitCastScalar<uint16_t>(prev_f16);
        for (; i < num_rows; ++i) {
            const uint16_t u16_delta = (static_cast<uint16_t>(in_b1[i]) << 8) | in_b0[i];
            prev_u16_scalar ^= u16_delta;
            feature_out[i] = hwy::ConvertScalarTo<float>(hwy::BitCastScalar<hwy::float16_t>(prev_u16_scalar));
        }

        if (num_rows > 0) {
            prev_row_state[f] = feature_out[num_rows - 1];
        }
    }
}

HWY_NOINLINE void XorFloat32_2D(const float* HWY_RESTRICT soa_data, const float* HWY_RESTRICT prev_row,
                                float* HWY_RESTRICT out, size_t num_rows, size_t num_features) {
    if (num_rows == 0) return;
    const size_t f32_lanes = hn::Lanes(d32);

    for (size_t f = 0; f < num_features; ++f) {
        const float* HWY_RESTRICT feature_col_in = soa_data + f * num_rows;
        float* HWY_RESTRICT feature_col_out = out + f * num_rows;

        const uint32_t u32_curr = hwy::BitCastScalar<uint32_t>(feature_col_in[0]);
        const uint32_t u32_prev = hwy::BitCastScalar<uint32_t>(prev_row[f]);
        feature_col_out[0] = hwy::BitCastScalar<float>(u32_curr ^ u32_prev);

        size_t i = 1;
#if HWY_TARGET != HWY_SCALAR
        for (; i + f32_lanes <= num_rows; i += f32_lanes) {
            const VF32 v_curr_f32 = hn::LoadU(d32, feature_col_in + i);
            const VF32 v_prev_f32 = hn::LoadU(d32, feature_col_in + i - 1); // Safe due to i=1 start
            const VU32 v_curr_u32 = hn::BitCast(du32, v_curr_f32);
            const VU32 v_prev_u32 = hn::BitCast(du32, v_prev_f32);
            const VU32 v_xor_u32 = hn::Xor(v_curr_u32, v_prev_u32);
            hn::StoreU(hn::BitCast(d32, v_xor_u32), d32, feature_col_out + i);
        }
#endif

        for (; i < num_rows; ++i) {
            const uint32_t u32_c = hwy::BitCastScalar<uint32_t>(feature_col_in[i]);
            const uint32_t u32_p = hwy::BitCastScalar<uint32_t>(feature_col_in[i - 1]); // Safe due to i=1 start
            feature_col_out[i] = hwy::BitCastScalar<float>(u32_c ^ u32_p);
        }
    }
}

HWY_NOINLINE void ShuffleFloat32_2D(const float* HWY_RESTRICT in, uint8_t* HWY_RESTRICT out,
                                   size_t num_rows, size_t num_features) {
    const size_t total_elements = num_rows * num_features;
    const hn::Repartition<uint16_t, decltype(du32)> d_u16_from_u32;
    const hn::Repartition<uint8_t, decltype(du16)> d_u8_from_u16;
    const size_t lanes_u32 = hn::Lanes(d32);
    const size_t step = 4 * lanes_u32;

    for (size_t f = 0; f < num_features; ++f) {
        const float* HWY_RESTRICT feature_col_in = in + f * num_rows;
        uint8_t* HWY_RESTRICT out_b0 = out + (0 * total_elements) + (f * num_rows);
        uint8_t* HWY_RESTRICT out_b1 = out + (1 * total_elements) + (f * num_rows);
        uint8_t* HWY_RESTRICT out_b2 = out + (2 * total_elements) + (f * num_rows);
        uint8_t* HWY_RESTRICT out_b3 = out + (3 * total_elements) + (f * num_rows);

        size_t i = 0;
#if HWY_TARGET != HWY_SCALAR
        for (; i + step <= num_rows; i += step) {
            const VU32 v_in_a = hn::BitCast(du32, hn::LoadU(d32, feature_col_in + i + 0 * lanes_u32));
            const VU32 v_in_b = hn::BitCast(du32, hn::LoadU(d32, feature_col_in + i + 1 * lanes_u32));
            const VU32 v_in_c = hn::BitCast(du32, hn::LoadU(d32, feature_col_in + i + 2 * lanes_u32));
            const VU32 v_in_d = hn::BitCast(du32, hn::LoadU(d32, feature_col_in + i + 3 * lanes_u32));

            const VU16 lo16_ab = hn::OrderedTruncate2To(d_u16_from_u32, v_in_a, v_in_b);
            const VU16 hi16_ab = hn::OrderedTruncate2To(d_u16_from_u32, hn::ShiftRight<16>(v_in_a), hn::ShiftRight<16>(v_in_b));
            const VU16 lo16_cd = hn::OrderedTruncate2To(d_u16_from_u32, v_in_c, v_in_d);
            const VU16 hi16_cd = hn::OrderedTruncate2To(d_u16_from_u32, hn::ShiftRight<16>(v_in_c), hn::ShiftRight<16>(v_in_d));

            const VU16 u16_lo_mask = hn::Set(du16, 0x00FF);
            const VU16 p0_ab = hn::And(lo16_ab, u16_lo_mask);
            const VU16 p1_ab = hn::ShiftRight<8>(lo16_ab);
            const VU16 p2_ab = hn::And(hi16_ab, u16_lo_mask);
            const VU16 p3_ab = hn::ShiftRight<8>(hi16_ab);

            const VU16 p0_cd = hn::And(lo16_cd, u16_lo_mask);
            const VU16 p1_cd = hn::ShiftRight<8>(lo16_cd);
            const VU16 p2_cd = hn::And(hi16_cd, u16_lo_mask);
            const VU16 p3_cd = hn::ShiftRight<8>(hi16_cd);

            const auto plane0 = hn::OrderedTruncate2To(d_u8_from_u16, p0_ab, p0_cd);
            const auto plane1 = hn::OrderedTruncate2To(d_u8_from_u16, p1_ab, p1_cd);
            const auto plane2 = hn::OrderedTruncate2To(d_u8_from_u16, p2_ab, p2_cd);
            const auto plane3 = hn::OrderedTruncate2To(d_u8_from_u16, p3_ab, p3_cd);

            hn::StoreU(plane0, d_u8_from_u16, out_b0 + i);
            hn::StoreU(plane1, d_u8_from_u16, out_b1 + i);
            hn::StoreU(plane2, d_u8_from_u16, out_b2 + i);
            hn::StoreU(plane3, d_u8_from_u16, out_b3 + i);
        }
#endif
        // Scalar remainder loop
        for (; i < num_rows; ++i) {
            const uint32_t val = hwy::BitCastScalar<uint32_t>(feature_col_in[i]);
            out_b0[i] = static_cast<uint8_t>((val      ) & 0xFF);
            out_b1[i] = static_cast<uint8_t>((val >> 8 ) & 0xFF);
            out_b2[i] = static_cast<uint8_t>((val >> 16) & 0xFF);
            out_b3[i] = static_cast<uint8_t>((val >> 24) & 0xFF);
        }
    }
}

HWY_NOINLINE void UnshuffleAndReconstruct32_2D(const uint8_t* HWY_RESTRICT shuffled_in, float* HWY_RESTRICT out,
                                               size_t num_rows, size_t num_features, std::span<float> prev_row_state) {
    const size_t total_elements = num_rows * num_features;
    const hn::Repartition<uint8_t, decltype(du16)> d_u8_from_u16;
    const hn::Repartition<uint16_t, decltype(du8)> d_u16_from_u8;
    const hn::Repartition<uint32_t, decltype(du16)> d_u32_from_u16;
    const size_t lanes_u32 = hn::Lanes(d32);
    const size_t step = 4 * lanes_u32;

    for (size_t f = 0; f < num_features; ++f) {
        float* HWY_RESTRICT feature_out = out + f * num_rows;
        const uint8_t* HWY_RESTRICT in_b0 = shuffled_in + (0 * total_elements) + (f * num_rows);
        const uint8_t* HWY_RESTRICT in_b1 = shuffled_in + (1 * total_elements) + (f * num_rows);
        const uint8_t* HWY_RESTRICT in_b2 = shuffled_in + (2 * total_elements) + (f * num_rows);
        const uint8_t* HWY_RESTRICT in_b3 = shuffled_in + (3 * total_elements) + (f * num_rows);
        
        uint32_t prev_u32 = hwy::BitCastScalar<uint32_t>(prev_row_state[f]);

        size_t i = 0;
#if HWY_TARGET != HWY_SCALAR
        for (; i + step <= num_rows; i += step) {
            constexpr size_t kPrefetchLookahead = 2;
            if (i + (kPrefetchLookahead * step) < num_rows) {
                hwy::Prefetch(in_b0 + i + (kPrefetchLookahead * step));
                hwy::Prefetch(in_b1 + i + (kPrefetchLookahead * step));
                hwy::Prefetch(in_b2 + i + (kPrefetchLookahead * step));
                hwy::Prefetch(in_b3 + i + (kPrefetchLookahead * step));
            }

            const hn::Repartition<uint8_t, decltype(d_u8_from_u16)> d_u8_from_u16_rebind;
            const auto v_p0 = hn::LoadU(d_u8_from_u16_rebind, in_b0 + i);
            const auto v_p1 = hn::LoadU(d_u8_from_u16_rebind, in_b1 + i);
            const auto v_p2 = hn::LoadU(d_u8_from_u16_rebind, in_b2 + i);
            const auto v_p3 = hn::LoadU(d_u8_from_u16_rebind, in_b3 + i);

            const auto t0_ab = hn::ZipLower(d_u16_from_u8, v_p0, v_p1);
            const auto t1_ab = hn::ZipLower(d_u16_from_u8, v_p2, v_p3);
            const auto t0_cd = hn::ZipUpper(d_u16_from_u8, v_p0, v_p1);
            const auto t1_cd = hn::ZipUpper(d_u16_from_u8, v_p2, v_p3);

            const VU32 delta_a = hn::ZipLower(d_u32_from_u16, t0_ab, t1_ab);
            const VU32 delta_b = hn::ZipUpper(d_u32_from_u16, t0_ab, t1_ab);
            const VU32 delta_c = hn::ZipLower(d_u32_from_u16, t0_cd, t1_cd);
            const VU32 delta_d = hn::ZipUpper(d_u32_from_u16, t0_cd, t1_cd);

            auto InclusiveScan = [lanes_u32](VU32 v) {
                for (size_t dist = 1; dist < lanes_u32; dist *= 2) {
                    v = hn::Xor(v, hn::SlideUpLanes(du32, v, dist));
                }
                return v;
            };

            // --- START OF FIX ---
            // Correctly chain the serial dependency between vectors.
            VU32 recon_a = hn::Xor(InclusiveScan(delta_a), hn::Set(du32, prev_u32));
            uint32_t last_val = hn::ExtractLane(recon_a, lanes_u32 - 1);

            VU32 recon_b = hn::Xor(InclusiveScan(delta_b), hn::Set(du32, last_val));
            last_val = hn::ExtractLane(recon_b, lanes_u32 - 1);

            VU32 recon_c = hn::Xor(InclusiveScan(delta_c), hn::Set(du32, last_val));
            last_val = hn::ExtractLane(recon_c, lanes_u32 - 1);
            
            VU32 recon_d = hn::Xor(InclusiveScan(delta_d), hn::Set(du32, last_val));
            
            // The new prev_u32 for the *next* iteration of this outer loop
            prev_u32 = hn::ExtractLane(recon_d, lanes_u32 - 1);
            // --- END OF FIX ---

            hn::StoreU(hn::BitCast(d32, recon_a), d32, feature_out + i + 0 * lanes_u32);
            hn::StoreU(hn::BitCast(d32, recon_b), d32, feature_out + i + 1 * lanes_u32);
            hn::StoreU(hn::BitCast(d32, recon_c), d32, feature_out + i + 2 * lanes_u32);
            hn::StoreU(hn::BitCast(d32, recon_d), d32, feature_out + i + 3 * lanes_u32);
        }
#endif
        // Scalar remainder loop
        for (; i < num_rows; ++i) {
            const uint32_t u32_delta = (static_cast<uint32_t>(in_b3[i]) << 24) |
                                       (static_cast<uint32_t>(in_b2[i]) << 16) |
                                       (static_cast<uint32_t>(in_b1[i]) << 8)  |
                                       in_b0[i];
            prev_u32 ^= u32_delta;
            feature_out[i] = hwy::BitCastScalar<float>(prev_u32);
        }

        if (num_rows > 0) {
            prev_row_state[f] = feature_out[num_rows - 1];
        }
    }
}

HWY_NOINLINE void XorInt64_2D(const int64_t* HWY_RESTRICT soa_data, const int64_t* HWY_RESTRICT prev_row,
                             int64_t* HWY_RESTRICT out, size_t num_rows, size_t num_features) {
    if (num_rows == 0) return;
    for (size_t f = 0; f < num_features; ++f) {
        const int64_t* HWY_RESTRICT feature_col_in = soa_data + f * num_rows;
        int64_t* HWY_RESTRICT feature_col_out = out + f * num_rows;
        
        feature_col_out[0] = feature_col_in[0] ^ prev_row[f];

        size_t i = 1;
        const size_t lanes = hn::Lanes(di64);
#if HWY_TARGET != HWY_SCALAR
        for (; i + lanes <= num_rows; i += lanes) {
            const VI64 v_curr = hn::LoadU(di64, feature_col_in + i);
            const VI64 v_prev = hn::LoadU(di64, feature_col_in + i - 1); // Safe due to i=1 start
            hn::StoreU(hn::Xor(v_curr, v_prev), di64, feature_col_out + i);
        }
#endif
        // Scalar remainder loop
        for (; i < num_rows; ++i) {
            feature_col_out[i] = feature_col_in[i] ^ feature_col_in[i - 1]; // Safe due to i=1 start
        }
    }
}

HWY_NOINLINE void UnXorInt64_2D(const int64_t* HWY_RESTRICT delta, int64_t* HWY_RESTRICT out,
                                size_t num_rows, size_t num_features, std::span<int64_t> prev_row_state) {
    if (num_rows == 0) return;
    for (size_t f = 0; f < num_features; ++f) {
        const int64_t* feature_delta = delta + f * num_rows;
        int64_t* feature_out = out + f * num_rows;
        
        uint64_t prev_val_u64 = hwy::BitCastScalar<uint64_t>(prev_row_state[f]);
        
        for (size_t i = 0; i < num_rows; ++i) {
            const uint64_t u64_delta = hwy::BitCastScalar<uint64_t>(feature_delta[i]);
            prev_val_u64 = u64_delta ^ prev_val_u64;
            feature_out[i] = hwy::BitCastScalar<int64_t>(prev_val_u64);
        }
        prev_row_state[f] = hwy::BitCastScalar<int64_t>(prev_val_u64);
    }
}

} // namespace cryptodd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();


#if HWY_ONCE
namespace cryptodd {

// The namespace is defined in the hwy/foreach_target.h
namespace HWY_NAMESPACE {
}

namespace simd {
    // Define the dispatchers that the public API will call.
    // These are defined only once, and they select the best SIMD implementation at runtime.
    HWY_EXPORT(DemoteAndXor2D);
    HWY_NOINLINE void DemoteAndXor2D_dispatcher(const float* current, const float* prev, hwy::float16_t* out, size_t num_rows, size_t num_features) {
        HWY_DYNAMIC_DISPATCH(DemoteAndXor2D)(current, prev, out, num_rows, num_features);
    }

    HWY_EXPORT(ShuffleFloat16_2D);
    HWY_NOINLINE void ShuffleFloat16_2D_dispatcher(const hwy::float16_t* in, uint8_t* out, size_t num_rows, size_t num_features) {
        HWY_DYNAMIC_DISPATCH(ShuffleFloat16_2D)(in, out, num_rows, num_features);
    }

    HWY_EXPORT(UnshuffleAndReconstruct16_2D);
    HWY_NOINLINE void UnshuffleAndReconstruct16_2D_dispatcher(const uint8_t* shuffled_in, float* out, size_t num_rows, size_t num_features, std::span<float> prev_row_state) {
        HWY_DYNAMIC_DISPATCH(UnshuffleAndReconstruct16_2D)(shuffled_in, out, num_rows, num_features, prev_row_state);
    }

    HWY_EXPORT(XorFloat32_2D);
    HWY_NOINLINE void XorFloat32_2D_dispatcher(const float* current, const float* prev, float* out, size_t num_rows, size_t num_features) {
        HWY_DYNAMIC_DISPATCH(XorFloat32_2D)(current, prev, out, num_rows, num_features);
    }

    HWY_EXPORT(ShuffleFloat32_2D);
    HWY_NOINLINE void ShuffleFloat32_2D_dispatcher(const float* in, uint8_t* out, size_t num_rows, size_t num_features) {
        HWY_DYNAMIC_DISPATCH(ShuffleFloat32_2D)(in, out, num_rows, num_features);
    }

    HWY_EXPORT(UnshuffleAndReconstruct32_2D);
    HWY_NOINLINE void UnshuffleAndReconstruct32_2D_dispatcher(const uint8_t* shuffled_in, float* out, size_t num_rows, size_t num_features, std::span<float> prev_row_state) {
        HWY_DYNAMIC_DISPATCH(UnshuffleAndReconstruct32_2D)(shuffled_in, out, num_rows, num_features, prev_row_state);
    }

    HWY_EXPORT(XorInt64_2D);
    HWY_NOINLINE void XorInt64_2D_dispatcher(const int64_t* current, const int64_t* prev, int64_t* out, size_t num_rows, size_t num_features) {
        HWY_DYNAMIC_DISPATCH(XorInt64_2D)(current, prev, out, num_rows, num_features);
    }

    HWY_EXPORT(UnXorInt64_2D);
    HWY_NOINLINE void UnXorInt64_2D_dispatcher(const int64_t* delta, int64_t* out, size_t num_rows, size_t num_features, std::span<int64_t> prev_row_state) {
        HWY_DYNAMIC_DISPATCH(UnXorInt64_2D)(delta, out, num_rows, num_features, prev_row_state);
    }
} // namespace simd

} // namespace cryptodd
#endif // HWY_ONCE