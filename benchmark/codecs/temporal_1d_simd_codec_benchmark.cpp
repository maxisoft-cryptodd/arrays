#include "temporal_1d_simd_codec.h"
#include "zstd_compressor.h"
#include <benchmark/benchmark.h>
#include <numeric>
#include <random>
#include <vector>

template <typename T>
static std::vector<T> generate_random_1d_data(size_t num_elements) {
    std::vector<T> data(num_elements);
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

class Temporal1dSimdCodecBenchmark : public benchmark::Fixture {
public:
    using Codec = cryptodd::Temporal1dSimdCodec;

    void SetUp(const ::benchmark::State& state) override {
        const size_t num_elements = state.range(0);
        original_float_data = generate_random_1d_data<float>(num_elements);
        original_int64_data = generate_random_1d_data<int64_t>(num_elements);

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
    float initial_prev_element_float = 123.45f;
    int64_t initial_prev_element_int64 = 98765;
    cryptodd::Temporal1dSimdCodecWorkspace workspace_;
    std::unique_ptr<Codec> codec_;
};

// --- Float16 Benchmarks ---
BENCHMARK_DEFINE_F(Temporal1dSimdCodecBenchmark, Encode16_Xor_Shuffle)(benchmark::State& state) {
    for (auto _ : state) {
        auto result = codec_->encode16_Xor_Shuffle(original_float_data, initial_prev_element_float, workspace_);
        if (!result) {
            state.SkipWithError(result.error().c_str());
            return;
        }
        benchmark::DoNotOptimize(std::move(*result));
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * original_float_data.size() * sizeof(float));
}

BENCHMARK_DEFINE_F(Temporal1dSimdCodecBenchmark, Decode16_Xor_Shuffle)(benchmark::State& state) {
    const size_t num_elements = state.range(0);
    auto encode_result = codec_->encode16_Xor_Shuffle(original_float_data, initial_prev_element_float, workspace_);
    if (!encode_result) {
        state.SkipWithError(("Setup for decode failed during encode: " + encode_result.error()).c_str());
        return;
    }
    const cryptodd::memory::vector<std::byte> encoded = std::move(*encode_result);

    for (auto _ : state) {
        float decoder_prev_element = initial_prev_element_float;
        auto result = codec_->decode16_Xor_Shuffle(encoded, num_elements, decoder_prev_element);
        if (!result) {
            state.SkipWithError(result.error().c_str());
            return;
        }
        benchmark::DoNotOptimize(std::move(*result));
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * original_float_data.size() * sizeof(float));
}

// --- Float32 Benchmarks ---
BENCHMARK_DEFINE_F(Temporal1dSimdCodecBenchmark, Encode32_Xor_Shuffle)(benchmark::State& state) {
    for (auto _ : state) {
        auto result = codec_->encode32_Xor_Shuffle(original_float_data, initial_prev_element_float, workspace_);
        if (!result) {
            state.SkipWithError(result.error().c_str());
            return;
        }
        benchmark::DoNotOptimize(std::move(*result));
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * original_float_data.size() * sizeof(float));
}

BENCHMARK_DEFINE_F(Temporal1dSimdCodecBenchmark, Decode32_Xor_Shuffle)(benchmark::State& state) {
    const size_t num_elements = state.range(0);
    auto encode_result = codec_->encode32_Xor_Shuffle(original_float_data, initial_prev_element_float, workspace_);
    if (!encode_result) {
        state.SkipWithError(("Setup for decode failed during encode: " + encode_result.error()).c_str());
        return;
    }
    const cryptodd::memory::vector<std::byte> encoded = std::move(*encode_result);

    for (auto _ : state) {
        float decoder_prev_element = initial_prev_element_float;
        auto result = codec_->decode32_Xor_Shuffle(encoded, num_elements, decoder_prev_element);
        if (!result) {
            state.SkipWithError(result.error().c_str());
            return;
        }
        benchmark::DoNotOptimize(std::move(*result));
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * original_float_data.size() * sizeof(float));
}

// --- Int64 Benchmarks ---
BENCHMARK_DEFINE_F(Temporal1dSimdCodecBenchmark, Encode64_Xor)(benchmark::State& state) {
    for (auto _ : state) {
        auto result = codec_->encode64_Xor(original_int64_data, initial_prev_element_int64, workspace_);
        if (!result) {
            state.SkipWithError(result.error().c_str());
            return;
        }
        benchmark::DoNotOptimize(std::move(*result));
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * original_int64_data.size() * sizeof(int64_t));
}

BENCHMARK_DEFINE_F(Temporal1dSimdCodecBenchmark, Decode64_Xor)(benchmark::State& state) {
    const size_t num_elements = state.range(0);
    auto encode_result = codec_->encode64_Xor(original_int64_data, initial_prev_element_int64, workspace_);
    if (!encode_result) {
        state.SkipWithError(("Setup for decode failed during encode: " + encode_result.error()).c_str());
        return;
    }
    const cryptodd::memory::vector<std::byte> encoded = std::move(*encode_result);

    for (auto _ : state) {
        int64_t decoder_prev_element = initial_prev_element_int64;
        auto result = codec_->decode64_Xor(encoded, num_elements, decoder_prev_element);
        if (!result) {
            state.SkipWithError(result.error().c_str());
            return;
        }
        benchmark::DoNotOptimize(std::move(*result));
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * original_int64_data.size() * sizeof(int64_t));
}

BENCHMARK_DEFINE_F(Temporal1dSimdCodecBenchmark, Encode64_Delta)(benchmark::State& state) {
    for (auto _ : state) {
        auto result = codec_->encode64_Delta(original_int64_data, initial_prev_element_int64, workspace_);
        if (!result) {
            state.SkipWithError(result.error().c_str());
            return;
        }
        benchmark::DoNotOptimize(std::move(*result));
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * original_int64_data.size() * sizeof(int64_t));
}

BENCHMARK_DEFINE_F(Temporal1dSimdCodecBenchmark, Decode64_Delta)(benchmark::State& state) {
    const size_t num_elements = state.range(0);
    auto encode_result = codec_->encode64_Delta(original_int64_data, initial_prev_element_int64, workspace_);
    if (!encode_result) {
        state.SkipWithError(("Setup for decode failed during encode: " + encode_result.error()).c_str());
        return;
    }
    const cryptodd::memory::vector<std::byte> encoded = std::move(*encode_result);

    for (auto _ : state) {
        int64_t decoder_prev_element = initial_prev_element_int64;
        auto result = codec_->decode64_Delta(encoded, num_elements, decoder_prev_element);
        if (!result) {
            state.SkipWithError(result.error().c_str());
            return;
        }
        benchmark::DoNotOptimize(std::move(*result));
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * original_int64_data.size() * sizeof(int64_t));
}


BENCHMARK_REGISTER_F(Temporal1dSimdCodecBenchmark, Encode16_Xor_Shuffle)->RangeMultiplier(8)->Range(64, 16 * 1024);
BENCHMARK_REGISTER_F(Temporal1dSimdCodecBenchmark, Decode16_Xor_Shuffle)->RangeMultiplier(8)->Range(64, 16 * 1024);
BENCHMARK_REGISTER_F(Temporal1dSimdCodecBenchmark, Encode32_Xor_Shuffle)->RangeMultiplier(8)->Range(64, 16 * 1024);
BENCHMARK_REGISTER_F(Temporal1dSimdCodecBenchmark, Decode32_Xor_Shuffle)->RangeMultiplier(8)->Range(64, 16 * 1024);
BENCHMARK_REGISTER_F(Temporal1dSimdCodecBenchmark, Encode64_Xor)->RangeMultiplier(8)->Range(64, 16 * 1024);
BENCHMARK_REGISTER_F(Temporal1dSimdCodecBenchmark, Decode64_Xor)->RangeMultiplier(8)->Range(64, 16 * 1024);
BENCHMARK_REGISTER_F(Temporal1dSimdCodecBenchmark, Encode64_Delta)->RangeMultiplier(8)->Range(64, 16 * 1024);
BENCHMARK_REGISTER_F(Temporal1dSimdCodecBenchmark, Decode64_Delta)->RangeMultiplier(8)->Range(64, 16 * 1024);