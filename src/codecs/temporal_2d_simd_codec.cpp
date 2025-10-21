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
    
    for (size_t f = 0; f < num_features; ++f) {
        const hwy::float16_t* HWY_RESTRICT feature_col_in = in + f * num_rows;
        uint8_t* HWY_RESTRICT out_b0 = out + f * num_rows;
        uint8_t* HWY_RESTRICT out_b1 = out + total_elements + f * num_rows;

        size_t i = 0;

#if HWY_TARGET != HWY_SCALAR
        const hn::FixedTag<hwy::float16_t, 8> d16_128;
        const auto du16_128 = hn::Repartition<uint16_t, decltype(d16_128)>();
        const auto du8_128  = hn::Repartition<uint8_t, decltype(d16_128)>();
        const hn::FixedTag<uint8_t, 8> du8_64;

        for (; i + 8 <= num_rows; i += 8) {
            const auto v_u16 = hn::BitCast(du16_128, hn::LoadU(d16_128, feature_col_in + i));
            const auto lo_bytes = hn::OrderedTruncate2To(du8_128, v_u16, v_u16);
            const auto hi_bytes = hn::OrderedTruncate2To(du8_128, hn::ShiftRight<8>(v_u16), hn::ShiftRight<8>(v_u16));
            hn::StoreU(hn::ResizeBitCast(du8_64, lo_bytes), du8_64, out_b0 + i);
            hn::StoreU(hn::ResizeBitCast(du8_64, hi_bytes), du8_64, out_b1 + i);
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

    for (size_t f = 0; f < num_features; ++f) {
        float* HWY_RESTRICT feature_out = out + f * num_rows;
        const uint8_t* HWY_RESTRICT in_b0 = shuffled_in + f * num_rows;
        const uint8_t* HWY_RESTRICT in_b1 = shuffled_in + total_elements + f * num_rows;

        uint16_t prev_u16_scalar = hwy::BitCastScalar<uint16_t>(hwy::ConvertScalarTo<hwy::float16_t>(prev_row_state[f]));

        size_t i = 0;

#if HWY_TARGET != HWY_SCALAR
        const hn::FixedTag<hwy::float16_t, 8> d16_128;
        const auto du16_128 = hn::Repartition<uint16_t, decltype(d16_128)>();
        const auto du8_128  = hn::Repartition<uint8_t, decltype(d16_128)>();
        const hn::FixedTag<uint8_t, 8> du8_64;
        const hn::FixedTag<float, 4> d32_128;

        for (; i + 8 <= num_rows; i += 8) {
            const auto v_b0_64 = hn::LoadU(du8_64, in_b0 + i);
            const auto v_b1_64 = hn::LoadU(du8_64, in_b1 + i);
            const auto v_b0_128 = hn::ResizeBitCast(du8_128, v_b0_64);
            const auto v_b1_128 = hn::ResizeBitCast(du8_128, v_b1_64);

            const auto v_delta_u16 = hn::ZipLower(du16_128, v_b0_128, v_b1_128);

            auto v_scan = v_delta_u16;
            v_scan = hn::Xor(v_scan, hn::SlideUpLanes(du16_128, v_scan, 1));
            v_scan = hn::Xor(v_scan, hn::SlideUpLanes(du16_128, v_scan, 2));
            v_scan = hn::Xor(v_scan, hn::SlideUpLanes(du16_128, v_scan, 4));

            const auto v_prev_bcast = hn::Set(du16_128, prev_u16_scalar);
            const auto v_recon_u16 = hn::Xor(v_scan, v_prev_bcast);

            prev_u16_scalar = hn::ExtractLane(v_recon_u16, 7);

            const auto v_recon_f16 = hn::BitCast(d16_128, v_recon_u16);

            auto v_out_f32_lo = hn::PromoteLowerTo(d32_128, v_recon_f16);
            auto v_out_f32_hi = hn::PromoteUpperTo(d32_128, v_recon_f16);
            hn::StoreU(v_out_f32_lo, d32_128, feature_out + i);
            hn::StoreU(v_out_f32_hi, d32_128, feature_out + i + 4);
        }
#endif

        // Scalar remainder loop
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

    for (size_t f = 0; f < num_features; ++f) {
        const float* HWY_RESTRICT feature_col_in = in + f * num_rows;
        uint8_t* HWY_RESTRICT out_b0 = out + (0 * total_elements) + (f * num_rows);
        uint8_t* HWY_RESTRICT out_b1 = out + (1 * total_elements) + (f * num_rows);
        uint8_t* HWY_RESTRICT out_b2 = out + (2 * total_elements) + (f * num_rows);
        uint8_t* HWY_RESTRICT out_b3 = out + (3 * total_elements) + (f * num_rows);

        size_t i = 0;

#if HWY_TARGET != HWY_SCALAR
        const hn::FixedTag<float, 4> d32_128;
        const auto du32_128 = hn::Repartition<uint32_t, decltype(d32_128)>();
        const auto du16_128 = hn::Repartition<uint16_t, decltype(d32_128)>();
        const auto du8_128  = hn::Repartition<uint8_t, decltype(d32_128)>();
        const hn::FixedTag<uint8_t, 4> du8_32;

        for (; i + 4 <= num_rows; i += 4) {
            const auto v_in = hn::LoadU(d32_128, feature_col_in + i);

            // De-interleave floats into byte planes using SIMD operations.
            const auto v_u32 = hn::BitCast(du32_128, v_in);
            const auto w_lo = hn::OrderedTruncate2To(du16_128, v_u32, v_u32);
            const auto w_hi = hn::ShiftRight<16>(v_u32);
            const auto w_hi_trunc = hn::OrderedTruncate2To(du16_128, w_hi, w_hi);

            const auto b0 = hn::OrderedTruncate2To(du8_128, w_lo, w_lo);
            const auto b1 = hn::OrderedTruncate2To(du8_128, hn::ShiftRight<8>(w_lo), hn::ShiftRight<8>(w_lo));
            const auto b2 = hn::OrderedTruncate2To(du8_128, w_hi_trunc, w_hi_trunc);
            const auto b3 = hn::OrderedTruncate2To(du8_128, hn::ShiftRight<8>(w_hi_trunc), hn::ShiftRight<8>(w_hi_trunc));

            // Store the 4 resulting bytes for each plane.
            hn::StoreU(hn::ResizeBitCast(du8_32, b0), du8_32, out_b0 + i);
            hn::StoreU(hn::ResizeBitCast(du8_32, b1), du8_32, out_b1 + i);
            hn::StoreU(hn::ResizeBitCast(du8_32, b2), du8_32, out_b2 + i);
            hn::StoreU(hn::ResizeBitCast(du8_32, b3), du8_32, out_b3 + i);
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

    for (size_t f = 0; f < num_features; ++f) {
        float* HWY_RESTRICT feature_out = out + f * num_rows;
        const uint8_t* HWY_RESTRICT in_b0 = shuffled_in + (0 * total_elements) + (f * num_rows);
        const uint8_t* HWY_RESTRICT in_b1 = shuffled_in + (1 * total_elements) + (f * num_rows);
        const uint8_t* HWY_RESTRICT in_b2 = shuffled_in + (2 * total_elements) + (f * num_rows);
        const uint8_t* HWY_RESTRICT in_b3 = shuffled_in + (3 * total_elements) + (f * num_rows);

        uint32_t prev_u32 = hwy::BitCastScalar<uint32_t>(prev_row_state[f]);

        size_t i = 0;

#if HWY_TARGET != HWY_SCALAR
        const hn::FixedTag<float, 4> d32_128;
        const auto du32_128 = hn::Repartition<uint32_t, decltype(d32_128)>();
        const auto du16_128 = hn::Repartition<uint16_t, decltype(d32_128)>();
        const auto du8_128  = hn::Repartition<uint8_t, decltype(d32_128)>();
        const hn::FixedTag<uint8_t, 4> du8_32;

        for (; i + 4 <= num_rows; i += 4) {
            const auto v_b0 = hn::LoadU(du8_32, in_b0 + i);
            const auto v_b1 = hn::LoadU(du8_32, in_b1 + i);
            const auto v_b2 = hn::LoadU(du8_32, in_b2 + i);
            const auto v_b3 = hn::LoadU(du8_32, in_b3 + i);

            const auto v_p0 = hn::ResizeBitCast(du8_128, v_b0);
            const auto v_p1 = hn::ResizeBitCast(du8_128, v_b1);
            const auto v_p2 = hn::ResizeBitCast(du8_128, v_b2);
            const auto v_p3 = hn::ResizeBitCast(du8_128, v_b3);

            const auto t0 = hn::ZipLower(du16_128, v_p0, v_p1);
            const auto t1 = hn::ZipLower(du16_128, v_p2, v_p3);

            // **FIXED**: Use 'auto' to let the compiler deduce the correct Vec128 type.
            const auto v_delta_u32 = hn::ZipLower(du32_128, t0, t1);

            // **FIXED**: Use 'auto' for consistency.
            auto v_scan = v_delta_u32;
            v_scan = hn::Xor(v_scan, hn::SlideUpLanes(du32_128, v_scan, 1));
            v_scan = hn::Xor(v_scan, hn::SlideUpLanes(du32_128, v_scan, 2));

            // **FIXED**: Use 'auto' here as well.
            const auto v_prev_bcast = hn::Set(du32_128, prev_u32);
            const auto v_recon_u32 = hn::Xor(v_scan, v_prev_bcast);

            hn::StoreU(hn::BitCast(d32_128, v_recon_u32), d32_128, feature_out + i);

            prev_u32 = hn::ExtractLane(v_recon_u32, 3);
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
        const int64_t* HWY_RESTRICT feature_delta = delta + f * num_rows;
        int64_t* HWY_RESTRICT feature_out = out + f * num_rows;

        uint64_t prev_val_u64 = hwy::BitCastScalar<uint64_t>(prev_row_state[f]);

        size_t i = 0;

#if HWY_TARGET != HWY_SCALAR
        const hn::FixedTag<int64_t, 2> di64_128;
        const auto du64_128 = hn::Repartition<uint64_t, decltype(di64_128)>();

        for (; i + 2 <= num_rows; i += 2) {
            // Load 2 delta values
            const auto v_delta_u64 = hn::BitCast(du64_128, hn::LoadU(di64_128, feature_delta + i));

            // Perform an intra-vector prefix XOR scan: [d0, d1] -> [d0, d0^d1]
            auto v_scan = hn::Xor(v_delta_u64, hn::SlideUpLanes(du64_128, v_delta_u64, 1));

            // XOR with the previous state to get the final values
            const auto v_prev_bcast = hn::Set(du64_128, prev_val_u64);
            const auto v_recon_u64 = hn::Xor(v_scan, v_prev_bcast);

            // Store the 2 reconstructed values
            hn::StoreU(hn::BitCast(di64_128, v_recon_u64), di64_128, feature_out + i);

            // Update the previous value for the next iteration with the last element
            prev_val_u64 = hn::ExtractLane(v_recon_u64, 1);
        }
#endif

        // Scalar remainder loop for any leftover elements
        for (; i < num_rows; ++i) {
            const uint64_t u64_delta = hwy::BitCastScalar<uint64_t>(feature_delta[i]);
            prev_val_u64 = u64_delta ^ prev_val_u64;
            feature_out[i] = hwy::BitCastScalar<int64_t>(prev_val_u64);
        }

        if (num_rows > 0) {
            prev_row_state[f] = hwy::BitCastScalar<int64_t>(prev_val_u64);
        }
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
