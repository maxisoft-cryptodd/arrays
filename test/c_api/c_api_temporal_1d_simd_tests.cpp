#include "cryptodd/c_api.h"
#include "../test_helpers.h"
#include "nlohmann/json.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <functional>
#include <vector>
#include <string>
#include <numeric>
#include <cmath>
#include <cstdint>

#include "float_conversion_simd_codec.h" // For F16 conversion check

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace test_helpers {
// Generator for 1D float data suitable for temporal compression
template <typename T>
cryptodd::memory::vector<T> generate_temporal_1d_data(size_t size) {
    cryptodd::memory::vector<T> data(size);
    if constexpr (std::is_floating_point_v<T>) {
        T current_val = 1000.0;
        for (size_t i = 0; i < size; ++i) {
            data[i] = current_val;
            current_val += static_cast<T>((i % 5 - 2) * 0.01); // Small, somewhat random changes
        }
    } else if constexpr (std::is_integral_v<T>) {
        T current_val = 1678912345678901; // Timestamp-like
        for (size_t i = 0; i < size; ++i) {
            data[i] = current_val;
            current_val += (i % 100) + 1; // Small, increasing delta
        }
    }
    return data;
}
} // namespace test_helpers

struct TestConfig1D {
    std::string test_name;
    std::string codec;
    std::string dtype;
    std::vector<int64_t> shape;
    bool use_append;
};

class CApiTemporal1dSimdTest : public ::testing::TestWithParam<TestConfig1D> {
protected:
    fs::path test_filepath_;
    std::vector<char> response_buffer_;
    std::vector<std::unique_ptr<cdd_handle_t, std::function<void(cdd_handle_t*)>>> handles_to_cleanup_;

    void SetUp() override {
        response_buffer_.resize(65536, '\0');
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

    template <typename T>
    void run_workflow() {
        const auto& config = GetParam();
        test_filepath_ = generate_unique_test_filepath();

        const size_t total_elements = std::accumulate(config.shape.begin(), config.shape.end(), size_t{1}, std::multiplies<>());
        auto full_data = test_helpers::generate_temporal_1d_data<T>(total_elements);

        size_t elements1 = total_elements;
        size_t elements2 = 0;

        std::span<const T> data_part1(full_data);
        std::span<const T> data_part2;
        
        std::vector<int64_t> shape1 = config.shape;
        std::vector<int64_t> shape2 = {};

        if (config.use_append) {
            elements1 = total_elements / 2;
            elements2 = total_elements - elements1;
            data_part1 = std::span(full_data.data(), elements1);
            data_part2 = std::span(full_data.data() + elements1, elements2);
            shape1[0] = elements1;
            shape2 = config.shape;
            shape2[0] = elements2;
        }

        // --- WRITE PHASE ---
        {
            json write_config = {
                {"backend", {{"type", "File"}, {"mode", "WriteTruncate"}, {"path", test_filepath_.string()}}}
            };
            cdd_handle_t writer = create_context(write_config);
            ASSERT_GT(writer, 0);

            json store_req = {
                {"op_type", "StoreChunk"},
                {"data_spec", {{"dtype", config.dtype}, {"shape", shape1}}},
                {"encoding", {{"codec", config.codec}}}
            };

            auto store_res = execute_op(writer, store_req, std::as_bytes(data_part1));
            ASSERT_FALSE(store_res.is_null());
            const auto compressed_size = store_res["details"]["compressed_size"].template get<int64_t>();
            ASSERT_LT(compressed_size, data_part1.size_bytes());
            handles_to_cleanup_.pop_back(); // Close context
        }

        // --- APPEND PHASE ---
        if (config.use_append) {
            json append_config = {
                {"backend", {{"type", "File"}, {"mode", "WriteAppend"}, {"path", test_filepath_.string()}}}
            };
            cdd_handle_t appender = create_context(append_config);
            ASSERT_GT(appender, 0);

            json store_req = {
                {"op_type", "StoreChunk"},
                {"data_spec", {{"dtype", config.dtype}, {"shape", shape2}}},
                {"encoding", {{"codec", config.codec}}}
            };

            auto store_res = execute_op(appender, store_req, std::as_bytes(data_part2));
            ASSERT_FALSE(store_res.is_null());
            ASSERT_EQ(store_res["details"]["chunk_index"], 1);
            handles_to_cleanup_.pop_back(); // Close context
        }

        // --- READ PHASE ---
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
            ASSERT_EQ(inspect_res["chunk_summaries"][0]["shape"], shape1);
            if (config.use_append) {
                ASSERT_EQ(inspect_res["chunk_summaries"][1]["shape"], shape2);
            }

            cryptodd::memory::vector<std::byte> output_buffer(full_data.size() * sizeof(T));
            auto load_res = execute_op(reader, {{"op_type", "LoadChunks"}, {"selection", {{"type", "All"}}}}, {}, output_buffer);
            ASSERT_FALSE(load_res.is_null());
            ASSERT_EQ(load_res["bytes_written_to_output"], output_buffer.size());

            if (config.use_append) {
                ASSERT_EQ(load_res["final_shape"], config.shape);
            }

            auto loaded_data = reinterpret_cast<const T*>(output_buffer.data());
            
            bool is_f16_codec = config.codec.find("F16") != std::string::npos;
            
            if constexpr (std::is_floating_point_v<T>) {
                float tolerance = is_f16_codec ? 0.005f : 1e-6f; // F16 has lower precision
                size_t error_count = 0;

                cryptodd::memory::vector<T> expected_data = std::cref(full_data);

                if (is_f16_codec) {
                    auto f16_codec = cryptodd::FloatConversionSimdCodec();
                    auto f16_floats = f16_codec.convert_f32_to_f16(std::span<const float>(expected_data));
                    auto f32_floats = f16_codec.convert_f16_to_f32(f16_floats);
                    expected_data.assign(f32_floats.begin(), f32_floats.end());
                }

                for (size_t i = 0; i < full_data.size(); ++i) {
                    if (std::abs(expected_data[i] - loaded_data[i]) > tolerance) {
                        error_count++;
                        if (error_count <= 10) {
                            ADD_FAILURE() << "Mismatch at index " << i << ": Original=" << expected_data[i]
                                          << ", Loaded=" << loaded_data[i] << ", Diff=" << std::abs(expected_data[i] - loaded_data[i]);
                        }
                    }
                }
                ASSERT_EQ(error_count, 0) << "Total mismatches: " << error_count << " / " << full_data.size();
            } else { // Integral types
                ASSERT_EQ(0, std::memcmp(full_data.data(), loaded_data, full_data.size() * sizeof(T)));
            }
        }
    }
};

TEST_P(CApiTemporal1dSimdTest, FullWorkflow) {
    const auto& config = GetParam();
    if (config.dtype == "FLOAT32") {
        run_workflow<float>();
    } else if (config.dtype == "INT64") {
        run_workflow<int64_t>();
    } else {
        FAIL() << "Unsupported dtype for this test suite: " << config.dtype;
    }
}


INSTANTIATE_TEST_SUITE_P(
    Temporal1dSimdCases,
    CApiTemporal1dSimdTest,
    ::testing::Values(
        TestConfig1D{"F32_XorShuffle", "TEMPORAL_1D_SIMD_F32_XOR_SHUFFLE", "FLOAT32", {5000}, false},
        TestConfig1D{"F16_XorShuffle", "TEMPORAL_1D_SIMD_F16_XOR_SHUFFLE_AS_F32", "FLOAT32", {5000}, false},
        TestConfig1D{"I64_Xor", "TEMPORAL_1D_SIMD_I64_XOR", "INT64", {4000}, false},
        TestConfig1D{"I64_Delta", "TEMPORAL_1D_SIMD_I64_DELTA", "INT64", {4000}, false},
        TestConfig1D{"Append_F32_XorShuffle", "TEMPORAL_1D_SIMD_F32_XOR_SHUFFLE", "FLOAT32", {8000}, true},
        TestConfig1D{"Append_I64_Delta", "TEMPORAL_1D_SIMD_I64_DELTA", "INT64", {6000}, true}
    ),
    [](const ::testing::TestParamInfo<CApiTemporal1dSimdTest::ParamType>& info) {
        return info.param.test_name;
    }
);