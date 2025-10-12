#include "orderbook_simd_codec.h"
#include "zdict.h" // For dictionary training
#include "zstd.h"
#include "zstd_compressor.h" // Include the concrete compressor
#include <gtest/gtest.h>
#include <numeric>
#include <random>
#include <vector>

using OkxCodec = cryptodd::OkxObSimdCodec;

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
    OkxCodec::Snapshot initial_prev_snapshot{};
};

TEST_F(OkxObSimdCodecTest, FullPipelineRoundTrip_NoDictionary)
{
    // 1. Create a codec instance without a dictionary
    OkxCodec codec(std::make_unique<cryptodd::ZstdCompressor>());    cryptodd::OrderbookSimdCodecWorkspace workspace;

    // The decoder needs the same starting state to begin reconstruction.
    OkxCodec::Snapshot decoder_prev_snapshot = initial_prev_snapshot;

    // 2. Encode the data
    auto encoded_result = codec.encode16(original_data, initial_prev_snapshot, workspace);
    ASSERT_TRUE(encoded_result.has_value()) << encoded_result.error();
    auto& encoded_data = *encoded_result;
    ASSERT_FALSE(encoded_data.empty());

    // 3. Decode the data
    auto decoded_result = codec.decode16(encoded_data, num_snapshots, decoder_prev_snapshot);
    ASSERT_TRUE(decoded_result.has_value()) << decoded_result.error();
    auto& decoded_data = *decoded_result;

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
    auto dictBuffer = std::vector<std::byte>(original_data.size() * sizeof(float) / 15);
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
    OkxCodec codec(std::make_unique<cryptodd::ZstdCompressor>(dictBuffer));    cryptodd::OrderbookSimdCodecWorkspace workspace;

    // The decoder needs the same starting state to begin reconstruction.
    OkxCodec::Snapshot decoder_prev_snapshot = initial_prev_snapshot;

    // 2. Encode the data
    auto encoded_result = codec.encode16(original_data, initial_prev_snapshot, workspace);
    ASSERT_TRUE(encoded_result.has_value()) << encoded_result.error();
    auto& encoded_data = *encoded_result;
    ASSERT_FALSE(encoded_data.empty());

    // 3. Decode the data
    auto decoded_result = codec.decode16(encoded_data, num_snapshots, decoder_prev_snapshot);
    ASSERT_TRUE(decoded_result.has_value()) << decoded_result.error();
    auto& decoded_data = *decoded_result;

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

TEST_F(OkxObSimdCodecTest, FullPipelineRoundTrip_Float32)
{
    // 1. Create a codec instance
    OkxCodec codec(
std::make_unique<cryptodd::ZstdCompressor>());    cryptodd::OrderbookSimdCodecWorkspace workspace;

    // The decoder needs the same starting state to begin reconstruction.
    OkxCodec::Snapshot decoder_prev_snapshot = initial_prev_snapshot;

    // 2. Encode the data using the float32 pipeline
    auto encoded_result = codec.encode32(original_data, initial_prev_snapshot, workspace);
    ASSERT_TRUE(encoded_result.has_value()) << encoded_result.error();
    auto& encoded_data = *encoded_result;
    ASSERT_FALSE(encoded_data.empty());

    // 3. Decode the data
    auto decoded_result = codec.decode32(encoded_data, num_snapshots, decoder_prev_snapshot);
    ASSERT_TRUE(decoded_result.has_value()) << decoded_result.error();
    auto& decoded_data = *decoded_result;

    // 4. Verify the decoded data
    ASSERT_EQ(decoded_data.size(), original_data.size());

    // Since float32 is a lossless pipeline, we can check for exact equality.
    for (size_t i = 0; i < original_data.size(); ++i) {
        ASSERT_EQ(original_data[i], decoded_data[i]);
    }

    // 5. Verify the final state of the decoder's snapshot
    const float* last_original_snapshot_ptr = original_data.data() + (num_snapshots - 1) * OkxCodec::SnapshotFloats;
    for (size_t i = 0; i < OkxCodec::SnapshotFloats; ++i) {
        ASSERT_EQ(last_original_snapshot_ptr[i], decoder_prev_snapshot[i]);
    }
}