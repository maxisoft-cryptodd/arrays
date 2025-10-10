#include <gtest/gtest.h>
#include <vector>
#include <random>
#include <numeric>
#include "okx_ob_simd_codec.h"
#include "zstd_compressor.h" // Include the concrete compressor
#include "zstd.h"
#include "zdict.h" // For dictionary training

using OkxCodec = cryptodd::OkxObSimdCodecDefault;

// Helper function to generate random snapshot data
std::vector<float> generate_random_snapshots(size_t num_snapshots) {
    std::vector<float> data(num_snapshots * OkxCodec::SnapshotFloats);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> dis(-1000.0, 1000.0);
    for (float& val : data) {
        val = static_cast<float>(dis(gen));
    }
    return data;
}

class OkxObSimdCodecTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Generate a consistent set of data for all tests
        original_data = generate_random_snapshots(num_snapshots);

        // The state of the order book *before* this batch starts.
        // For the first chunk in a file, this would typically be all zeros.
        // Fill with some non-zero data to make the test more robust
        std::iota(initial_prev_snapshot.begin(), initial_prev_snapshot.end(), 0.5f);
    }

    // Test parameters
    static constexpr size_t num_snapshots = 16 * 32; // Use a multiple of vector lanes for cleaner testing

    // Test data
    std::vector<float> original_data;
    OkxCodec::OkxSnapshot initial_prev_snapshot{};
};

TEST_F(OkxObSimdCodecTest, FullPipelineRoundTrip_NoDictionary)
{
    // 1. Create a codec instance without a dictionary
    OkxCodec codec(std::make_unique<cryptodd::ZstdCompressor>());

    // The decoder needs the same starting state to begin reconstruction.
    OkxCodec::OkxSnapshot decoder_prev_snapshot = initial_prev_snapshot;

    // 2. Encode the data
    std::vector<uint8_t> encoded_data;
    ASSERT_NO_THROW(encoded_data = codec.encode(original_data, initial_prev_snapshot));
    ASSERT_FALSE(encoded_data.empty());

    // 3. Decode the data
    std::vector<float> decoded_data;
    ASSERT_NO_THROW(decoded_data = codec.decode(encoded_data, num_snapshots, decoder_prev_snapshot));

    // 4. Verify the decoded data
    ASSERT_EQ(decoded_data.size(), original_data.size());

    // Since float16 is a lossy conversion, we check for approximate equality.
    // The error is slightly higher than pure f16 conversion due to the XOR operations.
    for (size_t i = 0; i < original_data.size(); ++i) {
        ASSERT_NEAR(original_data[i], decoded_data[i], 0.5f);
    }

    // 5. Verify the final state of the decoder's snapshot
    // After decoding, `decoder_prev_snapshot` should have been updated to match the
    // state of the VERY LAST snapshot from the original data, ready for the next chunk.
    const float* last_original_snapshot_ptr = original_data.data() + (num_snapshots - 1) * OkxCodec::SnapshotFloats;
    for (size_t i = 0; i < OkxCodec::SnapshotFloats; ++i) {
        ASSERT_NEAR(last_original_snapshot_ptr[i], decoder_prev_snapshot[i], 0.5f);
    }
}

TEST_F(OkxObSimdCodecTest, FullPipelineRoundTrip_WithDictionary)
{
    auto dictBuffer = std::vector<uint8_t>(original_data.size() * sizeof(float) / 15);
    ASSERT_GT(dictBuffer.size(), 8);
    auto sampleSizes = std::vector<size_t>();
    sampleSizes.assign(original_data.size() / OkxCodec::SnapshotFloats, OkxCodec::SnapshotFloats * sizeof(float));
    ASSERT_GT(sampleSizes.size(), 80);
    const size_t dict_size = ZDICT_trainFromBuffer(
    dictBuffer.data(), dictBuffer.size(),
        original_data.data(), sampleSizes.data(), sampleSizes.size());

    ASSERT_FALSE(ZDICT_isError(dict_size));
    ASSERT_GT(dict_size, 0);
    dictBuffer.resize(dict_size);

    // 1. Create a codec instance without a dictionary
    OkxCodec codec(std::make_unique<cryptodd::ZstdCompressor>(dictBuffer));

    // The decoder needs the same starting state to begin reconstruction.
    OkxCodec::OkxSnapshot decoder_prev_snapshot = initial_prev_snapshot;

    // 2. Encode the data
    std::vector<uint8_t> encoded_data;
    ASSERT_NO_THROW(encoded_data = codec.encode(original_data, initial_prev_snapshot));
    ASSERT_FALSE(encoded_data.empty());

    // 3. Decode the data
    std::vector<float> decoded_data;
    ASSERT_NO_THROW(decoded_data = codec.decode(encoded_data, num_snapshots, decoder_prev_snapshot));

    // 4. Verify the decoded data
    ASSERT_EQ(decoded_data.size(), original_data.size());

    // Since float16 is a lossy conversion, we check for approximate equality.
    // The error is slightly higher than pure f16 conversion due to the XOR operations.
    for (size_t i = 0; i < original_data.size(); ++i) {
        ASSERT_NEAR(original_data[i], decoded_data[i], 0.5f);
    }

    // 5. Verify the final state of the decoder's snapshot
    // After decoding, `decoder_prev_snapshot` should have been updated to match the
    // state of the VERY LAST snapshot from the original data, ready for the next chunk.
    const float* last_original_snapshot_ptr = original_data.data() + (num_snapshots - 1) * OkxCodec::SnapshotFloats;
    for (size_t i = 0; i < OkxCodec::SnapshotFloats; ++i) {
        ASSERT_NEAR(last_original_snapshot_ptr[i], decoder_prev_snapshot[i], 0.5f);
    }
}