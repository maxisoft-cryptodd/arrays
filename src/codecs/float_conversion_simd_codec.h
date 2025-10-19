#pragma once

#include "hwy/aligned_allocator.h"
#include "../memory/aligned.h"

#include <cstddef>
#include <span>

namespace cryptodd {

// Define aligned vector types for consistent memory management.
using Float32AlignedVector = memory::AlignedVector<float, static_cast<std::size_t>(HWY_ALIGNMENT)>;
using Float16AlignedVector = memory::AlignedVector<hwy::float16_t, static_cast<std::size_t>(HWY_ALIGNMENT)>;
using BFloat16AlignedVector = memory::AlignedVector<hwy::bfloat16_t, static_cast<std::size_t>(HWY_ALIGNMENT)>;

namespace simd {
    // Dispatcher function declarations for the SIMD implementations.
    void ConvertF32ToF16_1D_dispatcher(const float* in, hwy::float16_t* out, size_t num_elements);
    void ConvertF16ToF32_1D_dispatcher(const hwy::float16_t* in, float* out, size_t num_elements);

    // bfloat16 support added
    void ConvertF32ToBF16_1D_dispatcher(const float* in, hwy::bfloat16_t* out, size_t num_elements);
    void ConvertBF16ToF32_1D_dispatcher(const hwy::bfloat16_t* in, float* out, size_t num_elements);
}

/**
 * @class FloatConversionSimdCodec
 * @brief A utility class for high-performance conversion between float32, float16, and bfloat16 types using Highway SIMD.
 *
 * This class does not perform any compression; it provides direct, optimized
 * conversion routines. It follows the same SIMD implementation pattern as
 * Temporal1dSimdCodec.
 */
class FloatConversionSimdCodec {
public:
    FloatConversionSimdCodec() = default;

    /**
     * @brief Converts a span of 32-bit floats to a vector of 16-bit floats.
     * @param data The input span of float32 values.
     * @return An aligned vector containing the converted hwy::float16_t values.
     */
    [[nodiscard]] Float16AlignedVector convert_f32_to_f16(std::span<const float> data) const;

    /**
     * @brief Converts a span of 16-bit floats to a vector of 32-bit floats.
     * @param data The input span of hwy::float16_t values.
     * @return An aligned vector containing the converted float32 values.
     */
    [[nodiscard]] Float32AlignedVector convert_f16_to_f32(std::span<const hwy::float16_t> data) const;

    /**
     * @brief Converts a span of 32-bit floats to a vector of bfloat16 values.
     * @param data The input span of float32 values.
     * @return An aligned vector containing the converted hwy::bfloat16_t values.
     */
    [[nodiscard]] BFloat16AlignedVector convert_f32_to_bf16(std::span<const float> data) const;

    /**
     * @brief Converts a span of bfloat16 values to a vector of 32-bit floats.
     * @param data The input span of hwy::bfloat16_t values.
     * @return An aligned vector containing the converted float32 values.
     */
    [[nodiscard]] Float32AlignedVector convert_bf16_to_f32(std::span<const hwy::bfloat16_t> data) const;
};


// =============================================================================
// Inline Implementations
// =============================================================================

inline Float16AlignedVector FloatConversionSimdCodec::convert_f32_to_f16(std::span<const float> data) const {
    if (data.empty()) {
        return {};
    }
    Float16AlignedVector out_data(data.size());
    simd::ConvertF32ToF16_1D_dispatcher(data.data(), out_data.data(), data.size());
    return out_data;
}

inline Float32AlignedVector FloatConversionSimdCodec::convert_f16_to_f32(std::span<const hwy::float16_t> data) const {
    if (data.empty()) {
        return {};
    }
    Float32AlignedVector out_data(data.size());
    simd::ConvertF16ToF32_1D_dispatcher(data.data(), out_data.data(), data.size());
    return out_data;
}

// bfloat16 support added
inline BFloat16AlignedVector FloatConversionSimdCodec::convert_f32_to_bf16(std::span<const float> data) const {
    if (data.empty()) {
        return {};
    }
    BFloat16AlignedVector out_data(data.size());
    simd::ConvertF32ToBF16_1D_dispatcher(data.data(), out_data.data(), data.size());
    return out_data;
}

inline Float32AlignedVector FloatConversionSimdCodec::convert_bf16_to_f32(std::span<const hwy::bfloat16_t> data) const {
    if (data.empty()) {
        return {};
    }
    Float32AlignedVector out_data(data.size());
    simd::ConvertBF16ToF32_1D_dispatcher(data.data(), out_data.data(), data.size());
    return out_data;
}

} // namespace cryptodd