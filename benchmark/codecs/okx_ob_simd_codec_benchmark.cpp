#include <benchmark/benchmark.h>
#include <vector>
#include <random>
#include <numeric>
#include "okx_ob_simd_codec.h"

// Helper function to generate random snapshot data, adapted from the gtest file.
static std::vector<float> generate_random_snapshots(size_t num_snapshots) {
    std::vector<float> data(num_snapshots * cryptodd::OKX_OB_SNAPSHOT_FLOATS);
    std::mt19937 gen(1337); // Use a fixed seed for reproducible benchmark data
    std::uniform_real_distribution<> dis(-1000.0, 1000.0);
    for (float& val : data) {
        val = static_cast<float>(dis(gen));
    }
    return data;
}

// A benchmark fixture to set up data for the codec benchmarks.
// This avoids regenerating data for every single timing loop.
class OkxObSimdCodecBenchmark : public benchmark::Fixture {
public:
    void SetUp(const ::benchmark::State& state) override {
        const size_t num_snapshots = state.range(0);
        original_data = generate_random_snapshots(num_snapshots);

        // Set up initial state, similar to the test.
        initial_prev_snapshot.fill(0.0f);
        std::iota(initial_prev_snapshot.begin(), initial_prev_snapshot.end(), 0.5f);
    }

    void TearDown(const ::benchmark::State& state) override {
        original_data.clear();
    }

protected:
    std::vector<float> original_data;
    cryptodd::OkxSnapshot initial_prev_snapshot;
};

// Benchmark for the encode function
BENCHMARK_DEFINE_F(OkxObSimdCodecBenchmark, Encode)(benchmark::State& state) {
    for (auto _ : state) {
        std::vector<uint8_t> encoded_data = cryptodd::OkxObSimdCodec::encode(original_data, initial_prev_snapshot);
        benchmark::DoNotOptimize(encoded_data);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * original_data.size() * sizeof(float));
    state.SetLabel("Snapshots: " + std::to_string(state.range(0)));
}

// Benchmark for the decode function
BENCHMARK_DEFINE_F(OkxObSimdCodecBenchmark, Decode)(benchmark::State& state) {
    const size_t num_snapshots = state.range(0);
    // Pre-encode the data once, so we only measure decoding time.
    std::vector<uint8_t> encoded_data = cryptodd::OkxObSimdCodec::encode(original_data, initial_prev_snapshot);

    for (auto _ : state) {
        // The decoder modifies its previous snapshot state, so we must reset it for each run.
        cryptodd::OkxSnapshot decoder_prev_snapshot = initial_prev_snapshot;
        std::vector<float> decoded_data = cryptodd::OkxObSimdCodec::decode(encoded_data, num_snapshots, decoder_prev_snapshot);
        benchmark::DoNotOptimize(decoded_data);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * original_data.size() * sizeof(float));
    state.SetLabel("Snapshots: " + std::to_string(state.range(0)));
}

// Register the benchmarks to run with a range of snapshot counts.
// This will test the codec with small, medium, and large batches of data.
BENCHMARK_REGISTER_F(OkxObSimdCodecBenchmark, Encode)
    ->RangeMultiplier(8)
    ->Range(16, 16 * 1024); // From 16 to 16k snapshots

BENCHMARK_REGISTER_F(OkxObSimdCodecBenchmark, Decode)
    ->RangeMultiplier(8)
    ->Range(16, 16 * 1024);

// It's good practice to have a main entry point for benchmarks.
BENCHMARK_MAIN();