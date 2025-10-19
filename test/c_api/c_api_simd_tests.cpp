#include "cryptodd/c_api.h"
#include "../helpers/orderbook_generator.h"
#include "../test_helpers.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <numeric>
#include <string>
#include <vector>
#include <hwy/base.h>
#include "float_conversion_simd_codec.h"

#if defined(__has_include)
  #if __has_include(<stdfloat>)
    #include <stdfloat>
  #endif
#endif


#if defined(__STDCPP_FLOAT16_T__) and __STDCPP_FLOAT16_T__ == 1
using float16_t = __STDCPP_FLOAT16_T__;
#else
using float16_t = hwy::float16_t;
#endif


namespace fs = std::filesystem;
using json = nlohmann::json;

// This structure defines the complete configuration for a single parameterized test run.
struct TestConfig {
    std::string test_name;
    std::string codec;
    std::function<test_helpers::OrderbookTestData(const test_helpers::OrderbookParams&)> generator;
    test_helpers::OrderbookParams params;
    bool use_append;
};

class CApiOrderbookSimdTest : public ::testing::TestWithParam<TestConfig> {
protected:
    fs::path test_filepath_;
    std::vector<char> response_buffer_;
    std::vector<std::unique_ptr<cdd_handle_t, std::function<void(cdd_handle_t*)>>> handles_to_cleanup_;

    void SetUp() override {
        response_buffer_.resize(65536, '\0'); // Increased for potentially large JSON responses
    }

    void TearDown() override {
        handles_to_cleanup_.clear();
        if (!test_filepath_.empty() && fs::exists(test_filepath_)) {
            try {
                fs::remove(test_filepath_);
            } catch (const fs::filesystem_error& e) {
                std::cerr << "Warning: Could not clean up test file " << test_filepath_ 
                          << ": " << e.what() << std::endl;
            }
        }
    }
    
    cdd_handle_t create_context(const json& config) {
        std::string config_str = config.dump();
        cdd_handle_t handle = cdd_context_create(config_str.c_str(), config_str.length());
        if (handle > 0) {
            auto deleter = [](cdd_handle_t* h) { if (h) cdd_context_destroy(*h); };
            auto ptr = std::unique_ptr<cdd_handle_t, decltype(deleter)>(new cdd_handle_t(handle), deleter);
            handles_to_cleanup_.emplace_back(std::move(ptr));
        }
        return handle;
    }

    json execute_op(cdd_handle_t handle, const json& request,
                    std::span<const std::byte> input_data = {},
                    std::span<std::byte> output_data = {}) {
        std::string request_str = request.dump();
        response_buffer_.assign(response_buffer_.size(), '\0');

        int64_t result_code = cdd_execute_op(
            handle, request_str.c_str(), request_str.length(),
            input_data.data(), input_data.size(),
            output_data.data(), output_data.size(),
            response_buffer_.data(), response_buffer_.size()
        );

        if (result_code != CDD_SUCCESS) {
            ADD_FAILURE() << "cdd_execute_op failed with code: " << result_code 
                          << " (" << cdd_error_message(result_code) << ").\nResponse: " 
                          << response_buffer_.data();
            return nullptr;
        }

        json response;
        try {
            response = json::parse(response_buffer_.data());
        } catch (const json::parse_error& e) {
            ADD_FAILURE() << "Failed to parse JSON response: " << e.what() 
                          << "\nResponse Text:\n" << response_buffer_.data();
            return nullptr;
        }

        if (response["status"] != "Success") {
            ADD_FAILURE() << "Operation returned non-success status.\nRequest:\n" 
                          << request.dump(2) << "\nResponse:\n" << response.dump(2);
            return nullptr;
        }
        
        return response["result"];
    }
};

// The single, powerful test body that runs for every configuration.
TEST_P(CApiOrderbookSimdTest, FullWorkflow) {
    const auto& config = GetParam();
    test_filepath_ = generate_unique_test_filepath();

    // 1. --- DATA GENERATION ---
    auto full_data_struct = config.generator(config.params);
    const auto& full_data = full_data_struct.data;
    const size_t time_steps = full_data_struct.time_steps;
    const size_t levels = full_data_struct.depth_levels_per_side;
    const size_t features = full_data_struct.features;

    size_t time_steps1 = time_steps;
    size_t time_steps2 = 0;
    
    std::span<const float> data_part1(full_data);
    std::span<const float> data_part2;

    if (config.use_append) {
        time_steps1 = time_steps / 2;
        time_steps2 = time_steps - time_steps1;
        size_t split_point = time_steps1 * levels * 2 * features;
        data_part1 = std::span(full_data.data(), split_point);
        data_part2 = std::span(full_data.data() + split_point, full_data.size() - split_point);
    }
    
    // 2. --- WRITE PHASE ---
    {
        json write_config = {
            {"backend", {{"type", "File"}, {"mode", "WriteTruncate"}, {"path", test_filepath_.string()}}}
        };
        cdd_handle_t writer = create_context(write_config);
        ASSERT_GT(writer, 0);

        json store_req = {
            {"op_type", "StoreChunk"},
            {"data_spec", {{"dtype", "FLOAT32"}, {"shape", {time_steps1, levels * 2, features}}}},
            {"encoding", {{"codec", config.codec}}}
        };
        
        auto store_res = execute_op(writer, store_req, std::as_bytes(data_part1));
        ASSERT_FALSE(store_res.is_null());
        ASSERT_LT(store_res["details"]["compressed_size"].get<int64_t>(), data_part1.size_bytes());
        handles_to_cleanup_.pop_back();
    }

    // 3. --- APPEND PHASE (if enabled) ---
    if (config.use_append) {
        json append_config = {
            {"backend", {{"type", "File"}, {"mode", "WriteAppend"}, {"path", test_filepath_.string()}}}
        };
        cdd_handle_t appender = create_context(append_config);
        ASSERT_GT(appender, 0);

        json store_req = {
            {"op_type", "StoreChunk"},
            {"data_spec", {{"dtype", "FLOAT32"}, {"shape", {time_steps2, levels * 2, features}}}},
            {"encoding", {{"codec", config.codec}}}
        };

        auto store_res = execute_op(appender, store_req, std::as_bytes(data_part2));
        ASSERT_FALSE(store_res.is_null());
        ASSERT_EQ(store_res["details"]["chunk_index"], 1); // Verify it's the second chunk
        handles_to_cleanup_.pop_back();
    }

    // 4. --- READ AND VERIFY PHASE ---
    {
        json read_config = {
            {"backend", {{"type", "File"}, {"mode", "Read"}, {"path", test_filepath_.string()}}}
        };
        cdd_handle_t reader = create_context(read_config);
        ASSERT_GT(reader, 0);

        auto inspect_res = execute_op(reader, {{"op_type", "Inspect"}});
        ASSERT_FALSE(inspect_res.is_null());
        
        size_t expected_chunks = config.use_append ? 2 : 1;
        ASSERT_EQ(inspect_res["total_chunks"], expected_chunks);
        ASSERT_EQ(inspect_res["chunk_summaries"][0]["shape"], json::array({time_steps1, levels * 2, features}));
        if (config.use_append) {
            ASSERT_EQ(inspect_res["chunk_summaries"][1]["shape"], json::array({time_steps2, levels * 2, features}));
        }

        cryptodd::memory::vector<std::byte> output_buffer(full_data.size() * sizeof(float));
        auto load_res = execute_op(reader, {{"op_type", "LoadChunks"}, {"selection", {{"type", "All"}}}}, {}, output_buffer);
        ASSERT_FALSE(load_res.is_null());
        ASSERT_EQ(load_res["bytes_written_to_output"], output_buffer.size());
        
        if (config.use_append) {
            ASSERT_EQ(load_res["final_shape"], json::array({time_steps, levels * 2, features}));
        }

        auto loaded_floats = reinterpret_cast<const float*>(output_buffer.data());
        bool is_f16_codec = config.codec.find("F16") != std::string::npos;
        float tolerance = is_f16_codec ? 0.05f : 1e-5f; // F16 has precision loss
        size_t error_count = 0;

        cryptodd::memory::vector<float> downcasted_floats = std::cref(full_data); // copy

        if (is_f16_codec)
        {
            // Need to downcast to float16 then cast back to float32 for comparison
            auto f16_codec = cryptodd::FloatConversionSimdCodec();
            auto f16_floats = f16_codec.convert_f32_to_f16(std::span<const float>(downcasted_floats));
            auto f32_floats = f16_codec.convert_f16_to_f32(f16_floats);
            downcasted_floats.assign(f32_floats.begin(), f32_floats.end());
        }

        for (size_t i = 0; i < full_data.size(); ++i) {
            if (std::abs(downcasted_floats[i] - loaded_floats[i]) > tolerance) {
                error_count++;
                if (error_count <= 10) { // Log first 10 errors
                    ADD_FAILURE() << "Mismatch at index " << i << ": Original=" << downcasted_floats[i]
                                  << ", Loaded=" << loaded_floats[i] << ", Diff=" << std::abs(downcasted_floats[i] - loaded_floats[i]);
                }
            }
        }
        ASSERT_EQ(error_count, 0) << "Total mismatches: " << error_count << " / " << full_data.size();
    }
}

// Instantiate the test suite with a wide variety of configurations.
INSTANTIATE_TEST_SUITE_P(
    OrderbookSimdCases,
    CApiOrderbookSimdTest,
    ::testing::Values(
        // --- Test different generators ---
        TestConfig{"HybridGen_F32", "GENERIC_OB_SIMD_F32", test_helpers::generate_hybrid_orderbook_data, {150, 25}, false},
        TestConfig{"ClaudeGen_F32", "GENERIC_OB_SIMD_F32", test_helpers::generate_claude_style_orderbook_data, {150, 25}, false},
        TestConfig{"DeepSeekGen_F32", "GENERIC_OB_SIMD_F32", test_helpers::generate_deepseek_style_orderbook_data, {150, 25}, false},

        // --- Test different codecs with standard data ---
        TestConfig{"GenericF32_Medium", "GENERIC_OB_SIMD_F32", test_helpers::generate_hybrid_orderbook_data, {200, 50}, false},
        TestConfig{"GenericF16_Medium", "GENERIC_OB_SIMD_F16_AS_F32", test_helpers::generate_hybrid_orderbook_data, {200, 50}, false},
        // --- Test different shapes ---
        TestConfig{"SmallShape_F32", "GENERIC_OB_SIMD_F32", test_helpers::generate_hybrid_orderbook_data, {30, 10}, false},
        // --- Test APPEND functionality ---
        TestConfig{"Append_GenericF32", "GENERIC_OB_SIMD_F32", test_helpers::generate_hybrid_orderbook_data, {250, 30}, true}
        ),
    [](const ::testing::TestParamInfo<CApiOrderbookSimdTest::ParamType>& info) {
        return info.param.test_name;
    }
);