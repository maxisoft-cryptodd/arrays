#include "float_conversion_simd_codec.h"

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "float_conversion_simd_codec.cpp"
#include "hwy/foreach_target.h"

#include <hwy/highway.h>
#include "hwy/cache_control.h"

// Standard Highway boilerplate to enable per-target compilation.
HWY_BEFORE_NAMESPACE();
namespace cryptodd::HWY_NAMESPACE
{

    namespace hn = hwy::HWY_NAMESPACE;

    // Define the SIMD vector tags and types we will use.
    inline constexpr hn::ScalableTag<float> d32;
    inline constexpr hn::ScalableTag<hwy::float16_t> d16;
    inline constexpr hn::ScalableTag<hwy::bfloat16_t> dbf16; // bfloat16 support added

    using VF32 = hn::Vec<decltype(d32)>;
    using VF16 = hn::Vec<decltype(d16)>;
    using VBF16 = hn::Vec<decltype(dbf16)>; // bfloat16 support added

    // =============================================================================
    // F32 <=> F16 Kernels
    // =============================================================================

    static HWY_NOINLINE void ConvertF32ToF16(const float* HWY_RESTRICT in, hwy::float16_t* HWY_RESTRICT out,
                                             size_t num_elements)
    {
        size_t i = 0;
#if HWY_TARGET != HWY_SCALAR
        const hn::Half<decltype(d16)> d16_half;
        const size_t f32_lanes = hn::Lanes(d32);
        const size_t f16_lanes = hn::Lanes(d16);

        for (; i + f16_lanes <= num_elements; i += f16_lanes)
        {
            const VF32 v_in_f32_lo = hn::LoadU(d32, in + i);
            const VF32 v_in_f32_hi = hn::LoadU(d32, in + i + f32_lanes);
            auto v_out_f16_lo = hn::DemoteTo(d16_half, v_in_f32_lo);
            auto v_out_f16_hi = hn::DemoteTo(d16_half, v_in_f32_hi);
            VF16 v_out_f16 = hn::Combine(d16, v_out_f16_hi, v_out_f16_lo);

            // Changed from StoreU to Stream to avoid cache pollution.
            hn::Stream(v_out_f16, d16, out + i);
        }
#endif
        for (; i < num_elements; ++i)
        {
            out[i] = hwy::ConvertScalarTo<hwy::float16_t>(in[i]);
        }

        // Ensure all buffered stream writes are committed before the function returns.
#if HWY_TARGET != HWY_SCALAR
        // Ensure all buffered stream writes are committed before the function returns.
        hwy::FlushStream();
#endif
    }

    static HWY_NOINLINE void ConvertF16ToF32(const hwy::float16_t* HWY_RESTRICT in, float* HWY_RESTRICT out,
                                             size_t num_elements)
    {
        size_t i = 0;
#if HWY_TARGET != HWY_SCALAR
        const size_t f16_lanes = hn::Lanes(d16);
        const size_t f32_lanes = hn::Lanes(d32);
        for (; i + f16_lanes <= num_elements; i += f16_lanes)
        {
            const VF16 v_in_f16 = hn::LoadU(d16, in + i);
            VF32 v_out_f32_lo = hn::PromoteLowerTo(d32, v_in_f16);
            VF32 v_out_f32_hi = hn::PromoteUpperTo(d32, v_in_f16);

            // Changed from StoreU to Stream to avoid cache pollution.
            hn::Stream(v_out_f32_lo, d32, out + i);
            hn::Stream(v_out_f32_hi, d32, out + i + f32_lanes);
        }
#endif
        for (; i < num_elements; ++i)
        {
            out[i] = hwy::ConvertScalarTo<float>(in[i]);
        }
#if HWY_TARGET != HWY_SCALAR
        // Ensure all buffered stream writes are committed before the function returns.
        hwy::FlushStream();
#endif
    }

    // =============================================================================
    // F32 <=> BF16 Kernels (bfloat16 support added)
    // =============================================================================

    static HWY_NOINLINE void ConvertF32ToBF16(const float* HWY_RESTRICT in, hwy::bfloat16_t* HWY_RESTRICT out,
                                              size_t num_elements)
    {
        size_t i = 0;
#if HWY_TARGET != HWY_SCALAR
        const hn::Half<decltype(dbf16)> dbf16_half;
        const size_t f32_lanes = hn::Lanes(d32);
        const size_t bf16_lanes = hn::Lanes(dbf16);

        for (; i + bf16_lanes <= num_elements; i += bf16_lanes)
        {
            const VF32 v_in_f32_lo = hn::LoadU(d32, in + i);
            const VF32 v_in_f32_hi = hn::LoadU(d32, in + i + f32_lanes);
            auto v_out_bf16_lo = hn::DemoteTo(dbf16_half, v_in_f32_lo);
            auto v_out_bf16_hi = hn::DemoteTo(dbf16_half, v_in_f32_hi);
            VBF16 v_out_bf16 = hn::Combine(dbf16, v_out_bf16_hi, v_out_bf16_lo);

            // Changed from StoreU to Stream to avoid cache pollution.
            hn::Stream(v_out_bf16, dbf16, out + i);
        }
#endif
        for (; i < num_elements; ++i)
        {
            out[i] = hwy::ConvertScalarTo<hwy::bfloat16_t>(in[i]);
        }

        // Ensure all buffered stream writes are committed before the function returns.
#if HWY_TARGET != HWY_SCALAR
        // Ensure all buffered stream writes are committed before the function returns.
        hwy::FlushStream();
#endif
    }

    static HWY_NOINLINE void ConvertBF16ToF32(const hwy::bfloat16_t* HWY_RESTRICT in, float* HWY_RESTRICT out,
                                              size_t num_elements)
    {
        size_t i = 0;
#if HWY_TARGET != HWY_SCALAR
        const size_t bf16_lanes = hn::Lanes(dbf16);
        const size_t f32_lanes = hn::Lanes(d32);
        for (; i + bf16_lanes <= num_elements; i += bf16_lanes)
        {
            const VBF16 v_in_bf16 = hn::LoadU(dbf16, in + i);
            VF32 v_out_f32_lo = hn::PromoteLowerTo(d32, v_in_bf16);
            VF32 v_out_f32_hi = hn::PromoteUpperTo(d32, v_in_bf16);

            // Changed from StoreU to Stream to avoid cache pollution.
            hn::Stream(v_out_f32_lo, d32, out + i);
            hn::Stream(v_out_f32_hi, d32, out + i + f32_lanes);
        }
#endif
        for (; i < num_elements; ++i)
        {
            out[i] = hwy::ConvertScalarTo<float>(in[i]);
        }

        // Ensure all buffered stream writes are committed before the function returns.
#if HWY_TARGET != HWY_SCALAR
        // Ensure all buffered stream writes are committed before the function returns.
        hwy::FlushStream();
#endif
    }


    // Exportable wrapper functions to be called by the dispatcher.
    HWY_NOINLINE void ConvertF32ToF16_1D(const float* HWY_RESTRICT in, hwy::float16_t* HWY_RESTRICT out,
                                         size_t num_elements)
    {
        ConvertF32ToF16(in, out, num_elements);
    }

    HWY_NOINLINE void ConvertF16ToF32_1D(const hwy::float16_t* HWY_RESTRICT in, float* HWY_RESTRICT out,
                                         size_t num_elements)
    {
        ConvertF16ToF32(in, out, num_elements);
    }

    // bfloat16 support added
    HWY_NOINLINE void ConvertF32ToBF16_1D(const float* HWY_RESTRICT in, hwy::bfloat16_t* HWY_RESTRICT out,
                                          size_t num_elements)
    {
        ConvertF32ToBF16(in, out, num_elements);
    }

    HWY_NOINLINE void ConvertBF16ToF32_1D(const hwy::bfloat16_t* HWY_RESTRICT in, float* HWY_RESTRICT out,
                                          size_t num_elements)
    {
        ConvertBF16ToF32(in, out, num_elements);
    }

} // namespace cryptodd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
// =============================================================================
// Export and Dispatcher Definitions
// =============================================================================
namespace cryptodd
{

    // Required by the HWY_EXPORT macro for name lookup.
    namespace HWY_NAMESPACE {}

    namespace simd
    {
        HWY_EXPORT(ConvertF32ToF16_1D);
        HWY_NOINLINE void ConvertF32ToF16_1D_dispatcher(const float* in, hwy::float16_t* out, size_t num_elements)
        {
            HWY_DYNAMIC_DISPATCH(ConvertF32ToF16_1D)(in, out, num_elements);
        }

        HWY_EXPORT(ConvertF16ToF32_1D);
        HWY_NOINLINE void ConvertF16ToF32_1D_dispatcher(const hwy::float16_t* in, float* out, size_t num_elements)
        {
            HWY_DYNAMIC_DISPATCH(ConvertF16ToF32_1D)(in, out, num_elements);
        }

        // bfloat16 support added
        HWY_EXPORT(ConvertF32ToBF16_1D);
        HWY_NOINLINE void ConvertF32ToBF16_1D_dispatcher(const float* in, hwy::bfloat16_t* out, size_t num_elements)
        {
            HWY_DYNAMIC_DISPATCH(ConvertF32ToBF16_1D)(in, out, num_elements);
        }

        HWY_EXPORT(ConvertBF16ToF32_1D);
        HWY_NOINLINE void ConvertBF16ToF32_1D_dispatcher(const hwy::bfloat16_t* in, float* out, size_t num_elements)
        {
            HWY_DYNAMIC_DISPATCH(ConvertBF16ToF32_1D)(in, out, num_elements);
        }
    } // namespace simd
} // namespace cryptodd

#endif