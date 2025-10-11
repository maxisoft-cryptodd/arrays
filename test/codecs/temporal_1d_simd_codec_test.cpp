#include "temporal_1d_simd_codec.h"
#include "zstd_compressor.h"
#include <gtest/gtest.h>
#include <numeric>
#include <random>
#include <vector>

using Codec1D = cryptodd::Temporal1dSimdCodec;

template <typename T>
std::vector<T> generate_random_1d_data(size_t num_elements) {
    std::vector<T> data(num_elements);
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

class Temporal1dSimdCodecTest : public ::testing::Test {
protected:
    void SetUp() override {
        original_float_data = generate_random_1d_data<float>(kNumElements);
        original_int64_data = generate_random_1d_data<int64_t>(kNumElements);
    }

    static constexpr size_t kNumElements = 16 * 1024;

    std::vector<float> original_float_data;
    std::vector<int64_t> original_int64_data;
    float initial_prev_element_float = 123.45f;
    int64_t initial_prev_element_int64 = 98765;
};

TEST_F(Temporal1dSimdCodecTest, FullPipeline_Float16_Xor_Shuffle) {
    Codec1D codec(std::make_unique<cryptodd::ZstdCompressor>());
    cryptodd::Temporal1dSimdCodecWorkspace workspace;

    float decoder_prev_element = initial_prev_element_float;

    // Encode
    std::vector<uint8_t> encoded_data = codec.encode16_Xor_Shuffle(original_float_data, initial_prev_element_float, workspace);
    ASSERT_FALSE(encoded_data.empty());

    // Decode
    std::vector<float> decoded_data = codec.decode16_Xor_Shuffle(encoded_data, kNumElements, decoder_prev_element);

    // Verify data
    ASSERT_EQ(decoded_data.size(), original_float_data.size());
    for (size_t i = 0; i < original_float_data.size(); ++i) {
        ASSERT_NEAR(original_float_data[i], decoded_data[i], 0.5f);
    }

    // Verify final state
    ASSERT_NEAR(original_float_data.back(), decoder_prev_element, 0.5f);
}

TEST_F(Temporal1dSimdCodecTest, FullPipeline_Float32_Xor_Shuffle) {
    Codec1D codec(std::make_unique<cryptodd::ZstdCompressor>());
    cryptodd::Temporal1dSimdCodecWorkspace workspace;

    float decoder_prev_element = initial_prev_element_float;

    // Encode
    std::vector<uint8_t> encoded_data = codec.encode32_Xor_Shuffle(original_float_data, initial_prev_element_float, workspace);
    ASSERT_FALSE(encoded_data.empty());

    // Decode
    std::vector<float> decoded_data = codec.decode32_Xor_Shuffle(encoded_data, kNumElements, decoder_prev_element);

    // Verify data (lossless)
    ASSERT_EQ(decoded_data.size(), original_float_data.size());
    EXPECT_EQ(decoded_data, original_float_data);

    // Verify final state
    ASSERT_EQ(original_float_data.back(), decoder_prev_element);
}

TEST_F(Temporal1dSimdCodecTest, FullPipeline_Int64_Xor) {
    Codec1D codec(std::make_unique<cryptodd::ZstdCompressor>());
    cryptodd::Temporal1dSimdCodecWorkspace workspace;

    int64_t decoder_prev_element = initial_prev_element_int64;

    // Encode
    std::vector<uint8_t> encoded_data = codec.encode64_Xor(original_int64_data, initial_prev_element_int64, workspace);
    ASSERT_FALSE(encoded_data.empty());

    // Decode
    std::vector<int64_t> decoded_data = codec.decode64_Xor(encoded_data, kNumElements, decoder_prev_element);

    // Verify data (lossless)
    ASSERT_EQ(decoded_data.size(), original_int64_data.size());
    EXPECT_EQ(decoded_data, original_int64_data);

    // Verify final state
    ASSERT_EQ(original_int64_data.back(), decoder_prev_element);
}

TEST_F(Temporal1dSimdCodecTest, FullPipeline_Int64_Delta) {
    Codec1D codec(std::make_unique<cryptodd::ZstdCompressor>());
    cryptodd::Temporal1dSimdCodecWorkspace workspace;

    int64_t decoder_prev_element = initial_prev_element_int64;

    // Encode
    std::vector<uint8_t> encoded_data = codec.encode64_Delta(original_int64_data, initial_prev_element_int64, workspace);
    ASSERT_FALSE(encoded_data.empty());

    // Decode
    std::vector<int64_t> decoded_data = codec.decode64_Delta(encoded_data, kNumElements, decoder_prev_element);

    // Verify data (lossless)
    ASSERT_EQ(decoded_data.size(), original_int64_data.size());
    EXPECT_EQ(decoded_data, original_int64_data);

    // Verify final state
    ASSERT_EQ(original_int64_data.back(), decoder_prev_element);
}