#include "gtest/gtest.h"
#include "float_conversion_simd_codec.h"
#include "../helpers/orderbook_generator.h" // For realistic test data
#include <random>

// Using declarations to match the style of the provided test file.
using Codec = cryptodd::FloatConversionSimdCodec;
using namespace cryptodd;

/**
 * @brief Generates a vector of random data, mirroring the helper from the original test file.
 * @tparam T The data type (e.g., float).
 * @param num_elements The number of elements to generate.
 * @return A memory::vector<T> filled with random data.
 */
template <typename T>
auto generate_random_1d_data(size_t num_elements) {
    // Note: Assuming `memory::vector` is an alias for a std::vector or similar.
    // If it's a specific class, its header would be needed. For this test, std::vector is sufficient.
    memory::vector<T> data(num_elements);
    std::random_device rd;
    std::mt19937 gen(rd());

    if constexpr (std::is_floating_point_v<T>) {
        // Use a wide range to test different magnitudes.
        std::uniform_real_distribution<> dis(-50000.0, 50000.0);
        for (T& val : data) {
            val = static_cast<T>(dis(gen));
        }
    }
    return data;
}

/**
 * @class FloatConversionSimdCodecTest
 * @brief Test fixture for the FloatConversionSimdCodec.
 *
 * Sets up original float data that will be used as the source for all conversion tests.
 */
class FloatConversionSimdCodecTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Generate a large dataset to ensure vectorized loops are heavily used.
        original_float_data = generate_random_1d_data<float>(kNumElements);

        // Generate realistic, non-uniform data from the orderbook generator.
        test_helpers::OrderbookParams params;
        params.time_steps = 1024;
        params.depth_levels = 16;
        auto orderbook_test_data = test_helpers::generate_hybrid_orderbook_data(params);
        original_orderbook_data = std::move(orderbook_test_data.data);
    }

    static constexpr size_t kNumElements = 16 * 1024;

    memory::vector<float> original_float_data;
    memory::vector<float> original_orderbook_data;
};

// =============================================================================
// Float16 Round-trip Tests
// =============================================================================

TEST_F(FloatConversionSimdCodecTest, RoundTrip_Float16_WithRandomData) {
    Codec codec;

    // 1. Forward conversion: float32 -> float16
    Float16AlignedVector f16_data = codec.convert_f32_to_f16(original_float_data);
    ASSERT_EQ(f16_data.size(), original_float_data.size());

    // 2. Backward conversion: float16 -> float32
    Float32AlignedVector decoded_data = codec.convert_f16_to_f32(f16_data);
    ASSERT_EQ(decoded_data.size(), original_float_data.size());

    // 3. Verification
    // The conversion to float16 is lossy. To verify correctness, we must compare
    // against the expected result after a scalar round-trip conversion.
    for (size_t i = 0; i < original_float_data.size(); ++i) {
        const float original = original_float_data[i];
        const hwy::float16_t expected_f16 = hwy::ConvertScalarTo<hwy::float16_t>(original);
        const float expected_f32 = hwy::ConvertScalarTo<float>(expected_f16);
        ASSERT_FLOAT_EQ(decoded_data[i], expected_f32);
    }
}

TEST_F(FloatConversionSimdCodecTest, RoundTrip_Float16_WithOrderbookData) {
    Codec codec;

    Float16AlignedVector f16_data = codec.convert_f32_to_f16(original_orderbook_data);
    ASSERT_EQ(f16_data.size(), original_orderbook_data.size());

    Float32AlignedVector decoded_data = codec.convert_f16_to_f32(f16_data);
    ASSERT_EQ(decoded_data.size(), original_orderbook_data.size());

    for (size_t i = 0; i < original_orderbook_data.size(); ++i) {
        const float original = original_orderbook_data[i];
        const hwy::float16_t expected_f16 = hwy::ConvertScalarTo<hwy::float16_t>(original);
        const float expected_f32 = hwy::ConvertScalarTo<float>(expected_f16);
        ASSERT_FLOAT_EQ(decoded_data[i], expected_f32);
    }
}

// =============================================================================
// BFloat16 Round-trip Tests
// =============================================================================

TEST_F(FloatConversionSimdCodecTest, RoundTrip_BFloat16_WithRandomData) {
    Codec codec;

    // 1. Forward conversion: float32 -> bfloat16
    BFloat16AlignedVector bf16_data = codec.convert_f32_to_bf16(original_float_data);
    ASSERT_EQ(bf16_data.size(), original_float_data.size());

    // 2. Backward conversion: bfloat16 -> float32
    Float32AlignedVector decoded_data = codec.convert_bf16_to_f32(bf16_data);
    ASSERT_EQ(decoded_data.size(), original_float_data.size());

    // 3. Verification
    // bfloat16 is also lossy. We verify against the scalar round-trip expectation.
    for (size_t i = 0; i < original_float_data.size(); ++i) {
        const float original = original_float_data[i];
        const hwy::bfloat16_t expected_bf16 = hwy::ConvertScalarTo<hwy::bfloat16_t>(original);
        const float expected_f32 = hwy::ConvertScalarTo<float>(expected_bf16);
        // Using ASSERT_FLOAT_EQ ensures correct comparison for floating point numbers.
        ASSERT_FLOAT_EQ(decoded_data[i], expected_f32);
    }
}

TEST_F(FloatConversionSimdCodecTest, RoundTrip_BFloat16_WithOrderbookData) {
    Codec codec;

    BFloat16AlignedVector bf16_data = codec.convert_f32_to_bf16(original_orderbook_data);
    ASSERT_EQ(bf16_data.size(), original_orderbook_data.size());

    Float32AlignedVector decoded_data = codec.convert_bf16_to_f32(bf16_data);
    ASSERT_EQ(decoded_data.size(), original_orderbook_data.size());

    for (size_t i = 0; i < original_orderbook_data.size(); ++i) {
        const float original = original_orderbook_data[i];
        const hwy::bfloat16_t expected_bf16 = hwy::ConvertScalarTo<hwy::bfloat16_t>(original);
        const float expected_f32 = hwy::ConvertScalarTo<float>(expected_bf16);
        ASSERT_FLOAT_EQ(decoded_data[i], expected_f32);
    }
}