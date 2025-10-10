#include <benchmark/benchmark.h>
#include <vector>
#include <random>
#include <numeric>
#include "okx_ob_simd_codec.h"
#include "zstd_compressor.h" // Include the concrete compressor

// Helper function to generate random snapshot data, adapted from the gtest file.
static std::vector<float> generate_random_snapshots(size_t num_snapshots) {
    std::vector<float> data(num_snapshots * cryptodd::OkxObSimdCodecDefault::SnapshotFloats);
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

        // Create the codec with a standard ZstdCompressor for this benchmark run.
        codec_ = std::make_unique<cryptodd::OkxObSimdCodecDefault>(
            std::make_unique<cryptodd::ZstdCompressor>(-1)
        );
    }

    void TearDown(const ::benchmark::State& state) override {
        original_data.clear();
        codec_.reset();
    }

protected:
    std::vector<float> original_data;
    cryptodd::OkxObSimdCodecDefault::OkxSnapshot initial_prev_snapshot;
    std::unique_ptr<cryptodd::OkxObSimdCodecDefault> codec_;
};

// Benchmark for the encode function
BENCHMARK_DEFINE_F(OkxObSimdCodecBenchmark, Encode16)(benchmark::State& state) {
    for (auto _ : state) {
        // Call the encode method on the instance created in SetUp.
        std::vector<uint8_t> encoded_data = codec_->encode16(original_data, initial_prev_snapshot);
        benchmark::DoNotOptimize(encoded_data);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * original_data.size() * sizeof(float));
    state.SetLabel("Snapshots: " + std::to_string(state.range(0)));
}

// Benchmark for the decode function
BENCHMARK_DEFINE_F(OkxObSimdCodecBenchmark, Decode16)(benchmark::State& state) {
    const size_t num_snapshots = state.range(0);
    // Pre-encode the data once using the fixture's codec, so we only measure decoding time.
    std::vector<uint8_t> encoded_data = codec_->encode16(original_data, initial_prev_snapshot);

    for (auto _ : state) {
        // The decoder modifies its previous snapshot state, so we must reset it for each run.
        cryptodd::OkxObSimdCodecDefault::OkxSnapshot decoder_prev_snapshot = initial_prev_snapshot;
        // Call the decode method on the instance.
        std::vector<float> decoded_data = codec_->decode16(encoded_data, num_snapshots, decoder_prev_snapshot);
        benchmark::DoNotOptimize(decoded_data);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * original_data.size() * sizeof(float));
    state.SetLabel("Snapshots: " + std::to_string(state.range(0)));
}

// Register the benchmarks to run with a range of snapshot counts.
// This will test the codec with small, medium, and large batches of data.
BENCHMARK_REGISTER_F(OkxObSimdCodecBenchmark, Encode16)
    ->RangeMultiplier(8)
    ->Range(16, 16 * 1024); // From 16 to 16k snapshots

BENCHMARK_REGISTER_F(OkxObSimdCodecBenchmark, Decode16)
    ->RangeMultiplier(8)
    ->Range(16, 16 * 1024);

// --- Benchmarks for Float32 Pipeline ---

BENCHMARK_DEFINE_F(OkxObSimdCodecBenchmark, Encode32)(benchmark::State& state) {
    for (auto _ : state) {
        std::vector<uint8_t> encoded_data = codec_->encode32(original_data, initial_prev_snapshot);
        benchmark::DoNotOptimize(encoded_data);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * original_data.size() * sizeof(float));
    state.SetLabel("Snapshots: " + std::to_string(state.range(0)));
}

BENCHMARK_DEFINE_F(OkxObSimdCodecBenchmark, Decode32)(benchmark::State& state) {
    const size_t num_snapshots = state.range(0);
    std::vector<uint8_t> encoded_data = codec_->encode32(original_data, initial_prev_snapshot);

    for (auto _ : state) {
        cryptodd::OkxObSimdCodecDefault::OkxSnapshot decoder_prev_snapshot = initial_prev_snapshot;
        std::vector<float> decoded_data = codec_->decode32(encoded_data, num_snapshots, decoder_prev_snapshot);
        benchmark::DoNotOptimize(decoded_data);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * original_data.size() * sizeof(float));
    state.SetLabel("Snapshots: " + std::to_string(state.range(0)));
}

BENCHMARK_REGISTER_F(OkxObSimdCodecBenchmark, Encode32)->RangeMultiplier(8)->Range(16, 16 * 1024);
BENCHMARK_REGISTER_F(OkxObSimdCodecBenchmark, Decode32)->RangeMultiplier(8)->Range(16, 16 * 1024);


// It's good practice to have a main entry point for benchmarks.
BENCHMARK_MAIN();