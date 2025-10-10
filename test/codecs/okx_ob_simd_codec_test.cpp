#include <gtest/gtest.h>
#include <vector>
#include <random>
#include <numeric>
#include "okx_ob_simd_codec.h"

// Helper function to generate random snapshot data
std::vector<float> generate_random_snapshots(size_t num_snapshots) {
    std::vector<float> data(num_snapshots * cryptodd::OKX_OB_SNAPSHOT_FLOATS);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> dis(-1000.0, 1000.0);
    for (float& val : data) {
        val = static_cast<float>(dis(gen));
    }
    return data;
}

TEST(OkxObSimdCodecTest, FullPipelineRoundTrip) {
    // 1. Generate test data and initial state
    constexpr size_t num_snapshots = 16 * 32; // Use a multiple of vector lanes for cleaner testing
    const std::vector<float> original_data = generate_random_snapshots(num_snapshots);

    // The state of the order book *before* this batch starts.
    // For the first chunk in a file, this would typically be all zeros.
    cryptodd::OkxSnapshot initial_prev_snapshot{};
    // Fill with some non-zero data to make the test more robust
    std::iota(initial_prev_snapshot.begin(), initial_prev_snapshot.end(), 0.5f);

    // The decoder needs the same starting state to begin reconstruction.
    cryptodd::OkxSnapshot decoder_prev_snapshot = initial_prev_snapshot;

    // 2. Encode the data
    std::vector<uint8_t> encoded_data;
    ASSERT_NO_THROW(encoded_data = cryptodd::OkxObSimdCodec::encode(original_data, initial_prev_snapshot));
    ASSERT_FALSE(encoded_data.empty());

    // 3. Decode the data
    std::vector<float> decoded_data;
    ASSERT_NO_THROW(decoded_data = cryptodd::OkxObSimdCodec::decode(encoded_data, num_snapshots, decoder_prev_snapshot));

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
    const float* last_original_snapshot_ptr = original_data.data() + (num_snapshots - 1) * cryptodd::OKX_OB_SNAPSHOT_FLOATS;
    for (size_t i = 0; i < cryptodd::OKX_OB_SNAPSHOT_FLOATS; ++i) {
        ASSERT_NEAR(last_original_snapshot_ptr[i], decoder_prev_snapshot[i], 0.5f);
    }
}
