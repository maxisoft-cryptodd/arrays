#include "temporal_1d_simd_codec.h"

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "temporal_1d_simd_codec.cpp"
#include "hwy/foreach_target.h"

#include <hwy/highway.h>
#include "hwy/cache_control.h"

HWY_BEFORE_NAMESPACE();
namespace cryptodd::HWY_NAMESPACE {

namespace hn = hwy::HWY_NAMESPACE;

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
using VI64 = hn::Vec<decltype(di64)>;
using VU64 = hn::Vec<decltype(du64)>;
using VU8 = hn::Vec<decltype(du8)>;

// =================================================================================
// Self-contained SIMD kernels, adapted from orderbook_simd_codec.
// Made static to prevent linkage errors.
// =================================================================================

static HWY_NOINLINE void DemoteAndXor(const float* HWY_RESTRICT current, const float* HWY_RESTRICT prev,
                  hwy::float16_t* HWY_RESTRICT out, size_t num_floats) {
    size_t i = 0;
#if HWY_TARGET != HWY_SCALAR
    const hn::Half<decltype(d16)> d16_half;
    const size_t f32_lanes = hn::Lanes(d32);
    const size_t f16_lanes = hn::Lanes(d16);

    for (; i + f16_lanes <= num_floats; i += f16_lanes) {
        const VF32 v_curr_f32_lo = hn::LoadU(d32, current + i);
        const VF32 v_curr_f32_hi = hn::LoadU(d32, current + i + f32_lanes);
        const VF32 v_prev_f32_lo = hn::LoadU(d32, prev + i);
        const VF32 v_prev_f32_hi = hn::LoadU(d32, prev + i + f32_lanes);

        auto v_curr_f16_lo = hn::DemoteTo(d16_half, v_curr_f32_lo);
        auto v_curr_f16_hi = hn::DemoteTo(d16_half, v_curr_f32_hi);
        auto v_prev_f16_lo = hn::DemoteTo(d16_half, v_prev_f32_lo);
        auto v_prev_f16_hi = hn::DemoteTo(d16_half, v_prev_f32_hi);

        VF16 v_curr_f16 = hn::Combine(d16, v_curr_f16_hi, v_curr_f16_lo);
        VF16 v_prev_f16 = hn::Combine(d16, v_prev_f16_hi, v_prev_f16_lo);

        VU16 v_curr_u16 = hn::BitCast(du16, v_curr_f16);
        VU16 v_prev_u16 = hn::BitCast(du16, v_prev_f16);
        VU16 v_xor_u16 = hn::Xor(v_curr_u16, v_prev_u16);

        hn::StoreU(hn::BitCast(d16, v_xor_u16), d16, out + i);
    }
#endif
    for (; i < num_floats; ++i) {
        const hwy::float16_t f16_curr = hwy::ConvertScalarTo<hwy::float16_t>(current[i]);
        const hwy::float16_t f16_prev = hwy::ConvertScalarTo<hwy::float16_t>(prev[i]);
        const uint16_t u16_xor = hwy::BitCastScalar<uint16_t>(f16_curr) ^ hwy::BitCastScalar<uint16_t>(f16_prev);
        out[i] = hwy::BitCastScalar<hwy::float16_t>(u16_xor);
    }
}

static HWY_NOINLINE void ShuffleFloat16(const hwy::float16_t* HWY_RESTRICT in, uint8_t* HWY_RESTRICT out, size_t num_f16) {
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

        const auto lo_bytes = hn::OrderedTruncate2To(du8_128, v_u16, v_u16);
        const auto hi_bytes = hn::OrderedTruncate2To(du8_128, hn::ShiftRight<8>(v_u16), hn::ShiftRight<8>(v_u16));

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

static HWY_NOINLINE void XorFloat32(const float* HWY_RESTRICT current, const float* HWY_RESTRICT prev,
                float* HWY_RESTRICT out, size_t num_floats)
{
    size_t i = 0;
#if HWY_TARGET != HWY_SCALAR
    const size_t f32_lanes = hn::Lanes(d32);
    for (; i + f32_lanes <= num_floats; i += f32_lanes) {
        const VF32 v_curr_f32 = hn::LoadU(d32, current + i);
        const VF32 v_prev_f32 = hn::LoadU(d32, prev + i);
        const VU32 v_curr_u32 = hn::BitCast(du32, v_curr_f32);
        const VU32 v_prev_u32 = hn::BitCast(du32, v_prev_f32);
        const VU32 v_xor_u32 = hn::Xor(v_curr_u32, v_prev_u32);
        hn::StoreU(hn::BitCast(d32, v_xor_u32), d32, out + i);
    }
#endif
    for (; i < num_floats; ++i) {
        const uint32_t u32_curr = hwy::BitCastScalar<uint32_t>(current[i]);
        const uint32_t u32_prev = hwy::BitCastScalar<uint32_t>(prev[i]);
        const uint32_t u32_xor = u32_curr ^ u32_prev;
        out[i] = hwy::BitCastScalar<float>(u32_xor);
    }
}

static HWY_NOINLINE void ShuffleFloat32(const float* HWY_RESTRICT in, uint8_t* HWY_RESTRICT out, size_t num_f32) {
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
    const hn::FixedTag<uint8_t, 4> du8_32;

    for (; i + 4 <= num_f32; i += 4) {
        const auto v_in = hn::LoadU(d32_128, in + i);

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
    for (; i < num_f32; ++i) {
        const uint32_t val = hwy::BitCastScalar<uint32_t>(in[i]);
        out_b0[i] = static_cast<uint8_t>((val      ) & 0xFF);
        out_b1[i] = static_cast<uint8_t>((val >> 8 ) & 0xFF);
        out_b2[i] = static_cast<uint8_t>((val >> 16) & 0xFF);
        out_b3[i] = static_cast<uint8_t>((val >> 24) & 0xFF);
    }
}

HWY_NOINLINE void DemoteAndXor1D(const float* HWY_RESTRICT data, hwy::float16_t* HWY_RESTRICT out, size_t num_elements, float prev_element) {
    if (num_elements == 0) return;
    const hwy::float16_t f16_curr = hwy::ConvertScalarTo<hwy::float16_t>(data[0]);
    const hwy::float16_t f16_prev = hwy::ConvertScalarTo<hwy::float16_t>(prev_element);
    out[0] = hwy::BitCastScalar<hwy::float16_t>(static_cast<uint16_t>(hwy::BitCastScalar<uint16_t>(f16_curr) ^ hwy::BitCastScalar<uint16_t>(f16_prev)));
    if (num_elements > 1) {
        DemoteAndXor(data + 1, data, out + 1, num_elements - 1);
    }
}

HWY_NOINLINE void ShuffleFloat16_1D(const hwy::float16_t* HWY_RESTRICT in, uint8_t* HWY_RESTRICT out, size_t num_elements) {
    ShuffleFloat16(in, out, num_elements);
}

HWY_NOINLINE void UnshuffleAndReconstruct16_1D(const uint8_t* HWY_RESTRICT shuffled_in, float* HWY_RESTRICT out, size_t num_elements, float& prev_element) {
     if (num_elements == 0) return;
    const size_t total_floats = num_elements;
    const uint8_t* HWY_RESTRICT in_b0 = shuffled_in;
    const uint8_t* HWY_RESTRICT in_b1 = shuffled_in + total_floats;

    uint16_t prev_u16 = hwy::BitCastScalar<uint16_t>(hwy::ConvertScalarTo<hwy::float16_t>(prev_element));

    size_t i = 0;

#if HWY_TARGET != HWY_SCALAR
    const hn::FixedTag<hwy::float16_t, 8> d16_128;
    const auto du16_128 = hn::Repartition<uint16_t, decltype(d16_128)>();
    const auto du8_128  = hn::Repartition<uint8_t, decltype(d16_128)>();
    const hn::FixedTag<uint8_t, 8> du8_64;
    const hn::FixedTag<float, 4> d32_128;

    for (; i + 8 <= num_elements; i += 8) {
        const auto v_b0_64 = hn::LoadU(du8_64, in_b0 + i);
        const auto v_b1_64 = hn::LoadU(du8_64, in_b1 + i);
        const auto v_b0_128 = hn::ResizeBitCast(du8_128, v_b0_64);
        const auto v_b1_128 = hn::ResizeBitCast(du8_128, v_b1_64);

        const auto v_delta_u16 = hn::ZipLower(du16_128, v_b0_128, v_b1_128);

        // Inclusive prefix XOR scan on the 8 delta values
        auto v_scan = v_delta_u16;
        v_scan = hn::Xor(v_scan, hn::SlideUpLanes(du16_128, v_scan, 1));
        v_scan = hn::Xor(v_scan, hn::SlideUpLanes(du16_128, v_scan, 2));
        v_scan = hn::Xor(v_scan, hn::SlideUpLanes(du16_128, v_scan, 4));

        const auto v_prev_bcast = hn::Set(du16_128, prev_u16);
        const auto v_recon_u16 = hn::Xor(v_scan, v_prev_bcast);

        prev_u16 = hn::ExtractLane(v_recon_u16, 7);

        const auto v_recon_f16 = hn::BitCast(d16_128, v_recon_u16);

        auto v_out_f32_lo = hn::PromoteLowerTo(d32_128, v_recon_f16);
        auto v_out_f32_hi = hn::PromoteUpperTo(d32_128, v_recon_f16);
        hn::StoreU(v_out_f32_lo, d32_128, out + i);
        hn::StoreU(v_out_f32_hi, d32_128, out + i + 4);
    }
#endif

    // Scalar remainder loop
    for (; i < num_elements; ++i) {
        const uint16_t u16_delta = (static_cast<uint16_t>(in_b1[i]) << 8) | in_b0[i];
        prev_u16 ^= u16_delta;
        out[i] = hwy::ConvertScalarTo<float>(hwy::BitCastScalar<hwy::float16_t>(prev_u16));
    }

    if (num_elements > 0) {
        prev_element = out[num_elements - 1];
    }
}

HWY_NOINLINE void XorFloat32_1D(const float* HWY_RESTRICT data, float* HWY_RESTRICT out, size_t num_elements, float prev_element) {
    if (num_elements == 0) return;
    out[0] = hwy::BitCastScalar<float>(hwy::BitCastScalar<uint32_t>(data[0]) ^ hwy::BitCastScalar<uint32_t>(prev_element));
    if (num_elements > 1) {
        XorFloat32(data + 1, data, out + 1, num_elements - 1);
    }
}

HWY_NOINLINE void ShuffleFloat32_1D(const float* HWY_RESTRICT in, uint8_t* HWY_RESTRICT out, size_t num_elements) {
    ShuffleFloat32(in, out, num_elements);
}

HWY_NOINLINE void UnshuffleAndReconstruct32_1D(const uint8_t* HWY_RESTRICT shuffled_in, float* HWY_RESTRICT out, size_t num_elements, float& prev_element) {
    if (num_elements == 0) return;
    const uint8_t* HWY_RESTRICT in_b0 = shuffled_in;
    const uint8_t* HWY_RESTRICT in_b1 = shuffled_in + num_elements;
    const uint8_t* HWY_RESTRICT in_b2 = shuffled_in + 2 * num_elements;
    const uint8_t* HWY_RESTRICT in_b3 = shuffled_in + 3 * num_elements;

    uint32_t prev_u32 = hwy::BitCastScalar<uint32_t>(prev_element);

    size_t i = 0;

#if HWY_TARGET != HWY_SCALAR
    const hn::FixedTag<float, 4> d32_128;
    const auto du32_128 = hn::Repartition<uint32_t, decltype(d32_128)>();
    const auto du16_128 = hn::Repartition<uint16_t, decltype(d32_128)>();
    const auto du8_128  = hn::Repartition<uint8_t, decltype(d32_128)>();
    const hn::FixedTag<uint8_t, 4> du8_32;

    for (; i + 4 <= num_elements; i += 4) {
        // Unshuffle 4 bytes from each plane into a 128-bit vector of deltas
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
        const auto v_delta_u32 = hn::ZipLower(du32_128, t0, t1);

        // Perform an inclusive prefix XOR scan on the 4 delta values
        auto v_scan = v_delta_u32;
        v_scan = hn::Xor(v_scan, hn::SlideUpLanes(du32_128, v_scan, 1));
        v_scan = hn::Xor(v_scan, hn::SlideUpLanes(du32_128, v_scan, 2));

        // XOR the scanned results with the previous value to get the final values
        const auto v_prev_bcast = hn::Set(du32_128, prev_u32);
        const auto v_recon_u32 = hn::Xor(v_scan, v_prev_bcast);

        // Store the reconstructed floats
        hn::StoreU(hn::BitCast(d32_128, v_recon_u32), d32_128, out + i);

        // Update the previous value for the next iteration with the last element
        prev_u32 = hn::ExtractLane(v_recon_u32, 3);
    }
#endif

    // Scalar remainder loop
    for (; i < num_elements; ++i) {
        const uint32_t u32_delta = (static_cast<uint32_t>(in_b3[i]) << 24) |
                                   (static_cast<uint32_t>(in_b2[i]) << 16) |
                                   (static_cast<uint32_t>(in_b1[i]) << 8)  |
                                   in_b0[i];
        prev_u32 ^= u32_delta;
        out[i] = hwy::BitCastScalar<float>(prev_u32);
    }

    if (num_elements > 0) {
        prev_element = out[num_elements - 1];
    }
}

HWY_NOINLINE void XorInt64_1D(const int64_t* HWY_RESTRICT data, int64_t* HWY_RESTRICT out, size_t num_elements, int64_t prev_element) {
    if (num_elements == 0) return;
    out[0] = data[0] ^ prev_element;
    size_t i = 1;
    const size_t lanes = hn::Lanes(di64);
#if HWY_TARGET != HWY_SCALAR
    for (; i + lanes <= num_elements; i += lanes) {
        const VI64 v_curr = hn::LoadU(di64, data + i);
        const VI64 v_prev = hn::LoadU(di64, data + i - 1);
        hn::StoreU(hn::Xor(v_curr, v_prev), di64, out + i);
    }
#endif
    for (; i < num_elements; ++i) {
        out[i] = data[i] ^ data[i - 1];
    }
}

HWY_NOINLINE void UnXorInt64_1D(const int64_t* HWY_RESTRICT delta, int64_t* HWY_RESTRICT out, size_t num_elements, int64_t& prev_element) {
    if (num_elements == 0) return;
    uint64_t prev_u64 = hwy::BitCastScalar<uint64_t>(prev_element);
    size_t i = 0;
    const size_t lanes = hn::Lanes(du64);

#if HWY_TARGET != HWY_SCALAR
    for (; i + lanes <= num_elements; i += lanes) {
        const VU64 v_delta = hn::BitCast(du64, hn::LoadU(di64, delta + i));
        VU64 v_scan = v_delta;
        for (size_t dist = 1; dist < lanes; dist *= 2) {
            v_scan = hn::Xor(v_scan, hn::SlideUpLanes(du64, v_scan, dist));
        }
        VU64 v_recon = hn::Xor(v_scan, hn::Set(du64, prev_u64));
        hn::StoreU(hn::BitCast(di64, v_recon), di64, out + i);
        prev_u64 = hn::ExtractLane(v_recon, lanes - 1);
    }
#endif

    for (; i < num_elements; ++i) {
        prev_u64 = hwy::BitCastScalar<uint64_t>(delta[i]) ^ prev_u64;
        out[i] = hwy::BitCastScalar<int64_t>(prev_u64);
    }
    prev_element = hwy::BitCastScalar<int64_t>(prev_u64);
}

HWY_NOINLINE void DeltaInt64_1D(const int64_t* HWY_RESTRICT data, int64_t* HWY_RESTRICT out, size_t num_elements, int64_t prev_element) {
    if (num_elements == 0) return;
    out[0] = data[0] - prev_element;
    size_t i = 1;
    const size_t lanes = hn::Lanes(di64);
#if HWY_TARGET != HWY_SCALAR
    for (; i + lanes <= num_elements; i += lanes) {
        const VI64 v_curr = hn::LoadU(di64, data + i);
        const VI64 v_prev = hn::LoadU(di64, data + i - 1);
        hn::StoreU(hn::Sub(v_curr, v_prev), di64, out + i);
    }
#endif
    for (; i < num_elements; ++i) {
        out[i] = data[i] - data[i - 1];
    }
}

HWY_NOINLINE void CumulativeSumInt64_1D(const int64_t* HWY_RESTRICT delta, int64_t* HWY_RESTRICT out, size_t num_elements, int64_t& prev_element) {
    if (num_elements == 0) return;
    int64_t current_prev = prev_element;
    size_t i = 0;
    const size_t lanes = hn::Lanes(di64);

#if HWY_TARGET != HWY_SCALAR
    for (; i + lanes <= num_elements; i += lanes) {
        const VI64 v_delta = hn::LoadU(di64, delta + i);
        VI64 v_scan = v_delta;
        for (size_t dist = 1; dist < lanes; dist *= 2) {
            v_scan = hn::Add(v_scan, hn::SlideUpLanes(di64, v_scan, dist));
        }
        VI64 v_recon = hn::Add(v_scan, hn::Set(di64, current_prev));
        hn::StoreU(v_recon, di64, out + i);
        current_prev = hn::ExtractLane(v_recon, lanes - 1);
    }
#endif

    for (; i < num_elements; ++i) {
        current_prev += delta[i];
        out[i] = current_prev;
    }

    if (num_elements > 0) {
        prev_element = out[num_elements - 1];
    }
}

} // namespace cryptodd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace cryptodd {

namespace HWY_NAMESPACE {
}

namespace simd {
    HWY_EXPORT(DemoteAndXor1D);
    HWY_NOINLINE void DemoteAndXor1D_dispatcher(const float* data, hwy::float16_t* out, size_t num_elements, float prev_element) {
        HWY_DYNAMIC_DISPATCH(DemoteAndXor1D)(data, out, num_elements, prev_element);
    }

    HWY_EXPORT(ShuffleFloat16_1D);
    HWY_NOINLINE void ShuffleFloat16_1D_dispatcher(const hwy::float16_t* in, uint8_t* out, size_t num_elements) {
        HWY_DYNAMIC_DISPATCH(ShuffleFloat16_1D)(in, out, num_elements);
    }

    HWY_EXPORT(UnshuffleAndReconstruct16_1D);
    HWY_NOINLINE void UnshuffleAndReconstruct16_1D_dispatcher(const uint8_t* shuffled_in, float* out, size_t num_elements, float& prev_element) {
        HWY_DYNAMIC_DISPATCH(UnshuffleAndReconstruct16_1D)(shuffled_in, out, num_elements, prev_element);
    }

    HWY_EXPORT(XorFloat32_1D);
    HWY_NOINLINE void XorFloat32_1D_dispatcher(const float* data, float* out, size_t num_elements, float prev_element) {
        HWY_DYNAMIC_DISPATCH(XorFloat32_1D)(data, out, num_elements, prev_element);
    }

    HWY_EXPORT(ShuffleFloat32_1D);
    HWY_NOINLINE void ShuffleFloat32_1D_dispatcher(const float* in, uint8_t* out, size_t num_elements) {
        HWY_DYNAMIC_DISPATCH(ShuffleFloat32_1D)(in, out, num_elements);
    }

    HWY_EXPORT(UnshuffleAndReconstruct32_1D);
    HWY_NOINLINE void UnshuffleAndReconstruct32_1D_dispatcher(const uint8_t* shuffled_in, float* out, size_t num_elements, float& prev_element) {
        HWY_DYNAMIC_DISPATCH(UnshuffleAndReconstruct32_1D)(shuffled_in, out, num_elements, prev_element);
    }

    HWY_EXPORT(XorInt64_1D);
    HWY_NOINLINE void XorInt64_1D_dispatcher(const int64_t* data, int64_t* out, size_t num_elements, int64_t prev_element) {
        HWY_DYNAMIC_DISPATCH(XorInt64_1D)(data, out, num_elements, prev_element);
    }

    HWY_EXPORT(UnXorInt64_1D);
    HWY_NOINLINE void UnXorInt64_1D_dispatcher(const int64_t* delta, int64_t* out, size_t num_elements, int64_t& prev_element) {
        HWY_DYNAMIC_DISPATCH(UnXorInt64_1D)(delta, out, num_elements, prev_element);
    }

    HWY_EXPORT(DeltaInt64_1D);
    HWY_NOINLINE void DeltaInt64_1D_dispatcher(const int64_t* data, int64_t* out, size_t num_elements, int64_t prev_element) {
        HWY_DYNAMIC_DISPATCH(DeltaInt64_1D)(data, out, num_elements, prev_element);
    }

    HWY_EXPORT(CumulativeSumInt64_1D);
    HWY_NOINLINE void CumulativeSumInt64_1D_dispatcher(const int64_t* delta, int64_t* out, size_t num_elements, int64_t& prev_element) {
        HWY_DYNAMIC_DISPATCH(CumulativeSumInt64_1D)(delta, out, num_elements, prev_element);
    }
} // namespace simd

} // namespace cryptodd
#endif // HWY_ONCE
