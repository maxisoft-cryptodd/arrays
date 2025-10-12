#include "temporal_2d_simd_codec.h"
#include "zstd_compressor.h"
#include <gtest/gtest.h>
#include <numeric>
#include <random>
#include <vector>

using StaticCodec = cryptodd::Temporal2dSimdCodec<8>;
using DynamicCodec = cryptodd::DynamicTemporal2dSimdCodec;

template <typename T>
std::vector<T> generate_random_soa_data(size_t num_rows, size_t num_features) {
    std::vector<T> data(num_rows * num_features);
    std::random_device rd;
    std::mt19937 gen(rd());

    if constexpr (std::is_floating_point_v<T>) {
        std::uniform_real_distribution<> dis(-1000.0, 1000.0);
        for (T& val : data) {
            val = static_cast<T>(dis(gen));
        }
    } else {
        std::uniform_int_distribution<T> dis(0, 100000);
        for (T& val : data) {
            val = dis(gen);
        }
    }
    return data;
}

class Temporal2dSimdCodecTest : public ::testing::Test {
protected:
    void SetUp() override {
        original_float_data = generate_random_soa_data<float>(kNumRows, StaticCodec::kNumFeatures);
        original_int64_data = generate_random_soa_data<int64_t>(kNumRows, StaticCodec::kNumFeatures);

        std::iota(initial_prev_row_float.begin(), initial_prev_row_float.end(), 0.5f);
        std::iota(initial_prev_row_int64.begin(), initial_prev_row_int64.end(), 100);
    }

    static constexpr size_t kNumRows = 16 * 32;

    std::vector<float> original_float_data;
    std::vector<int64_t> original_int64_data;
    StaticCodec::PrevRowFloat initial_prev_row_float{};
    StaticCodec::PrevRowInt64 initial_prev_row_int64{};
};

// --- Static Codec Tests ---

TEST_F(Temporal2dSimdCodecTest, Static_FullPipelineRoundTrip_Float16) {
    StaticCodec codec(std::make_unique<cryptodd::ZstdCompressor>());
    cryptodd::Temporal2dSimdCodecWorkspace workspace;

    auto decoder_prev_row = initial_prev_row_float;

    // Encode
    auto encoded_result = codec.encode16(original_float_data, initial_prev_row_float, workspace);
    ASSERT_TRUE(encoded_result.has_value()) << encoded_result.error();
    auto& encoded_data = *encoded_result;
    ASSERT_FALSE(encoded_data.empty());

    // Decode
    auto decoded_result = codec.decode16(encoded_data, kNumRows, decoder_prev_row);
    ASSERT_TRUE(decoded_result.has_value()) << decoded_result.error();
    auto& decoded_data = *decoded_result;

    // Verify data
    ASSERT_EQ(decoded_data.size(), original_float_data.size());
    for (size_t i = 0; i < original_float_data.size(); ++i) {
        ASSERT_NEAR(original_float_data[i], decoded_data[i], 0.5f);
    }

    // Verify final state
    for (size_t f = 0; f < StaticCodec::kNumFeatures; ++f) {
        const float last_original_val = original_float_data[(f * kNumRows) + kNumRows - 1];
        ASSERT_NEAR(last_original_val, decoder_prev_row[f], 0.5f);
    }
}

TEST_F(Temporal2dSimdCodecTest, Static_FullPipelineRoundTrip_Float32) {
    StaticCodec codec(std::make_unique<cryptodd::ZstdCompressor>());
    cryptodd::Temporal2dSimdCodecWorkspace workspace;

    auto decoder_prev_row = initial_prev_row_float;

    // Encode
    auto encoded_result = codec.encode32(original_float_data, initial_prev_row_float, workspace);
    ASSERT_TRUE(encoded_result.has_value()) << encoded_result.error();
    auto& encoded_data = *encoded_result;
    ASSERT_FALSE(encoded_data.empty());

    // Decode
    auto decoded_result = codec.decode32(encoded_data, kNumRows, decoder_prev_row);
    ASSERT_TRUE(decoded_result.has_value()) << decoded_result.error();
    auto& decoded_data = *decoded_result;

    // Verify data (lossless)
    ASSERT_EQ(decoded_data.size(), original_float_data.size());
    for (size_t i = 0; i < original_float_data.size(); ++i) {
        ASSERT_EQ(original_float_data[i], decoded_data[i]);
    }

    // Verify final state
    for (size_t f = 0; f < StaticCodec::kNumFeatures; ++f) {
        const float last_original_val = original_float_data[(f * kNumRows) + kNumRows - 1];
        ASSERT_EQ(last_original_val, decoder_prev_row[f]);
    }
}

TEST_F(Temporal2dSimdCodecTest, Static_FullPipelineRoundTrip_Int64) {
    StaticCodec codec(std::make_unique<cryptodd::ZstdCompressor>());
    cryptodd::Temporal2dSimdCodecWorkspace workspace;

    auto decoder_prev_row = initial_prev_row_int64;

    // Encode
    auto encoded_result = codec.encode64(original_int64_data, initial_prev_row_int64, workspace);
    ASSERT_TRUE(encoded_result.has_value()) << encoded_result.error();
    auto& encoded_data = *encoded_result;
    ASSERT_FALSE(encoded_data.empty());

    // Decode
    auto decoded_result = codec.decode64(encoded_data, kNumRows, decoder_prev_row);
    ASSERT_TRUE(decoded_result.has_value()) << decoded_result.error();
    auto& decoded_data = *decoded_result;

    // Verify data (lossless)
    ASSERT_EQ(decoded_data.size(), original_int64_data.size());
    EXPECT_EQ(decoded_data, original_int64_data);

    // Verify final state
    for (size_t f = 0; f < StaticCodec::kNumFeatures; ++f) {
        const int64_t last_original_val = original_int64_data[(f * kNumRows) + kNumRows - 1];
        ASSERT_EQ(last_original_val, decoder_prev_row[f]);
    }
}

// --- Dynamic Codec Tests ---

TEST_F(Temporal2dSimdCodecTest, Dynamic_FullPipelineRoundTrip_Float32) {
    DynamicCodec codec(StaticCodec::kNumFeatures, std::make_unique<cryptodd::ZstdCompressor>());
    cryptodd::Temporal2dSimdCodecWorkspace workspace;

    std::vector<float> decoder_prev_row(initial_prev_row_float.begin(), initial_prev_row_float.end());

    // Encode
    auto encoded_result = codec.encode32(original_float_data, decoder_prev_row, workspace);
    ASSERT_TRUE(encoded_result.has_value()) << encoded_result.error();
    auto& encoded_data = *encoded_result;
    ASSERT_FALSE(encoded_data.empty());

    // Decode
    auto decoded_result = codec.decode32(encoded_data, decoder_prev_row);
    ASSERT_TRUE(decoded_result.has_value()) << decoded_result.error();
    auto& decoded_data = *decoded_result;

    // Verify data (lossless)
    ASSERT_EQ(decoded_data.size(), original_float_data.size());
    EXPECT_EQ(decoded_data, original_float_data);

    // Verify final state
    for (size_t f = 0; f < StaticCodec::kNumFeatures; ++f) {
        const float last_original_val = original_float_data[(f * kNumRows) + kNumRows - 1];
        ASSERT_EQ(last_original_val, decoder_prev_row[f]);
    }
}