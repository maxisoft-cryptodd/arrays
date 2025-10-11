#include "temporal_2d_simd_codec.h"
#include "zstd_compressor.h"
#include <benchmark/benchmark.h>
#include <numeric>
#include <random>
#include <vector>

template <typename T>
static std::vector<T> generate_random_soa_data(size_t num_rows, size_t num_features) {
    std::vector<T> data(num_rows * num_features);
    std::mt19937 gen(1337); // Fixed seed for reproducible benchmarks

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

class Temporal2dSimdCodecBenchmark : public benchmark::Fixture {
public:
    static constexpr size_t kNumFeatures = 8;
    using Codec = cryptodd::Temporal2dSimdCodec<kNumFeatures>;

    void SetUp(const ::benchmark::State& state) override {
        const size_t num_rows = state.range(0);
        original_float_data = generate_random_soa_data<float>(num_rows, kNumFeatures);
        original_int64_data = generate_random_soa_data<int64_t>(num_rows, kNumFeatures);

        std::iota(initial_prev_row_float.begin(), initial_prev_row_float.end(), 0.5f);
        std::iota(initial_prev_row_int64.begin(), initial_prev_row_int64.end(), 100);

        codec_ = std::make_unique<Codec>(std::make_unique<cryptodd::ZstdCompressor>(-1));
    }

    void TearDown(const ::benchmark::State& state) override {
        original_float_data.clear();
        original_int64_data.clear();
        codec_.reset();
    }

protected:
    std::vector<float> original_float_data;
    std::vector<int64_t> original_int64_data;
    Codec::PrevRowFloat initial_prev_row_float{};
    Codec::PrevRowInt64 initial_prev_row_int64{};
    cryptodd::Temporal2dSimdCodecWorkspace workspace_;
    std::unique_ptr<Codec> codec_;
};

// --- Float16 Benchmarks ---
BENCHMARK_DEFINE_F(Temporal2dSimdCodecBenchmark, Encode16)(benchmark::State& state) {
    for (auto _ : state) {
        std::vector<uint8_t> encoded = codec_->encode16(original_float_data, initial_prev_row_float, workspace_);
        benchmark::DoNotOptimize(encoded);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * original_float_data.size() * sizeof(float));
}

BENCHMARK_DEFINE_F(Temporal2dSimdCodecBenchmark, Decode16)(benchmark::State& state) {
    const size_t num_rows = state.range(0);
    std::vector<uint8_t> encoded = codec_->encode16(original_float_data, initial_prev_row_float, workspace_);
    for (auto _ : state) {
        auto decoder_prev_row = initial_prev_row_float;
        std::vector<float> decoded = codec_->decode16(encoded, num_rows, decoder_prev_row);
        benchmark::DoNotOptimize(decoded);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * original_float_data.size() * sizeof(float));
}

// --- Float32 Benchmarks ---
BENCHMARK_DEFINE_F(Temporal2dSimdCodecBenchmark, Encode32)(benchmark::State& state) {
    for (auto _ : state) {
        std::vector<uint8_t> encoded = codec_->encode32(original_float_data, initial_prev_row_float, workspace_);
        benchmark::DoNotOptimize(encoded);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * original_float_data.size() * sizeof(float));
}

BENCHMARK_DEFINE_F(Temporal2dSimdCodecBenchmark, Decode32)(benchmark::State& state) {
    const size_t num_rows = state.range(0);
    std::vector<uint8_t> encoded = codec_->encode32(original_float_data, initial_prev_row_float, workspace_);
    for (auto _ : state) {
        auto decoder_prev_row = initial_prev_row_float;
        std::vector<float> decoded = codec_->decode32(encoded, num_rows, decoder_prev_row);
        benchmark::DoNotOptimize(decoded);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * original_float_data.size() * sizeof(float));
}

// --- Int64 Benchmarks ---
BENCHMARK_DEFINE_F(Temporal2dSimdCodecBenchmark, Encode64)(benchmark::State& state) {
    for (auto _ : state) {
        std::vector<uint8_t> encoded = codec_->encode64(original_int64_data, initial_prev_row_int64, workspace_);
        benchmark::DoNotOptimize(encoded);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * original_int64_data.size() * sizeof(int64_t));
}

BENCHMARK_DEFINE_F(Temporal2dSimdCodecBenchmark, Decode64)(benchmark::State& state) {
    const size_t num_rows = state.range(0);
    std::vector<uint8_t> encoded = codec_->encode64(original_int64_data, initial_prev_row_int64, workspace_);
    for (auto _ : state) {
        auto decoder_prev_row = initial_prev_row_int64;
        std::vector<int64_t> decoded = codec_->decode64(encoded, num_rows, decoder_prev_row);
        benchmark::DoNotOptimize(decoded);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * original_int64_data.size() * sizeof(int64_t));
}

BENCHMARK_REGISTER_F(Temporal2dSimdCodecBenchmark, Encode16)->RangeMultiplier(8)->Range(64, 16 * 1024);
BENCHMARK_REGISTER_F(Temporal2dSimdCodecBenchmark, Decode16)->RangeMultiplier(8)->Range(64, 16 * 1024);
BENCHMARK_REGISTER_F(Temporal2dSimdCodecBenchmark, Encode32)->RangeMultiplier(8)->Range(64, 16 * 1024);
BENCHMARK_REGISTER_F(Temporal2dSimdCodecBenchmark, Decode32)->RangeMultiplier(8)->Range(64, 16 * 1024);
BENCHMARK_REGISTER_F(Temporal2dSimdCodecBenchmark, Encode64)->RangeMultiplier(8)->Range(64, 16 * 1024);
BENCHMARK_REGISTER_F(Temporal2dSimdCodecBenchmark, Decode64)->RangeMultiplier(8)->Range(64, 16 * 1024);

//BENCHMARK_MAIN();