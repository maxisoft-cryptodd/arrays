#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <numeric>
#include <string_view>
#include <vector>
#include "../test_helpers.h"
#include "base64.h" // For verifying metadata
#include "cryptodd/c_api.h"
#include "data_reader.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

class CApiTest : public ::testing::Test {
protected:
    fs::path test_filepath_;
    std::vector<char> response_buffer_;

    void SetUp() override {
        // Pre-allocate a generous buffer for JSON responses
        response_buffer_.resize(16384, '\0');
    }

    void TearDown() override {
        handles_to_cleanup_.clear();

        if (!test_filepath_.empty() && fs::exists(test_filepath_)) {
            try {
                fs::remove(test_filepath_);
            } catch (const fs::filesystem_error& e) {
                std::cerr << "Warning: Could not clean up test file " << test_filepath_ << ": " << e.what() << std::endl;
            }
        }
    }
    
    // Creates a handle and registers it for automatic cleanup.
    cdd_handle_t create_context(const json& config) {
        std::string config_str = config.dump();
        cdd_handle_t handle = cdd_context_create(config_str.c_str(), config_str.length());
        if (handle > 0) {
            // Basic RAII: use a custom deleter with a unique_ptr to manage the handle.
            // When the unique_ptr goes out of scope, the handle is destroyed.
            auto deleter = [](cdd_handle_t* h)
            {
                if (h)
                {
                    cdd_context_destroy(*h);
                }
            };
            auto ptr = std::unique_ptr<cdd_handle_t, decltype(deleter)>(new cdd_handle_t(handle), deleter);
            handles_to_cleanup_.emplace_back(std::move(ptr));
        }
        return handle;
    }

    json execute_op(
        cdd_handle_t handle,
        const json& request,
        std::span<const std::byte> input_data = {},
        std::span<std::byte> output_data = {}) {

        std::string request_str = request.dump();
        response_buffer_.assign(response_buffer_.size(), '\0');

        int64_t result_code = cdd_execute_op(
            handle,
            request_str.c_str(), request_str.length(),
            input_data.data(), input_data.size(),
            output_data.data(), output_data.size(),
            response_buffer_.data(), response_buffer_.size()
        );

        if (result_code != CDD_SUCCESS) {
            ADD_FAILURE() << "cdd_execute_op failed with code: " << result_code << " (" << cdd_error_message(result_code) << ").\nResponse: " << response_buffer_.data();
            return nullptr;
        }

        json response;
        try {
             response = json::parse(response_buffer_.data());
        } catch (const json::parse_error& e) {
            ADD_FAILURE() << "Failed to parse JSON response: " << e.what() << "\nResponse Text:\n" << response_buffer_.data();
            return nullptr;
        }

        if (response["status"] != "Success") {
            ADD_FAILURE() << "Operation returned non-success status.\nRequest:\n" << request.dump(2) << "\nResponse:\n" << response.dump(2);
            return nullptr;
        }
        
        return response["result"];
    }

protected:
    std::vector<std::unique_ptr<cdd_handle_t, std::function<void(cdd_handle_t*)>>> handles_to_cleanup_;
};

// --- Test Group 1: Lifecycle and Basic Errors ---

TEST_F(CApiTest, ContextLifecycleAndBasicErrors) {
    // Successful creation
    json mem_config = {{"backend", {{"type", "Memory"}, {"mode", "WriteTruncate"}}}};
    cdd_handle_t handle = create_context(mem_config);
    ASSERT_GT(handle, 0);

    // Destroying an invalid handle should fail
    ASSERT_EQ(cdd_context_destroy(99999), CDD_ERROR_INVALID_HANDLE);
    
    // Creation with invalid JSON should fail
    const char* bad_json = R"({"backend": {"type": "Memory" "mode": "Write"}})"; // missing comma
    cdd_handle_t bad_handle = cdd_context_create(bad_json, strlen(bad_json));
    ASSERT_EQ(bad_handle, CDD_ERROR_INVALID_JSON);

    // Creation with a non-existent file for reading should fail
    json bad_file_config = {{"backend", {{"type", "File"}, {"mode", "Read"}, {"path", "/non/existent/path/for/sure/file.cdd"}}}};
    bad_handle = create_context(bad_file_config);
    ASSERT_EQ(bad_handle, CDD_ERROR_RESOURCE_UNAVAILABLE);

    // Executing on an invalid handle should fail
    json ping_req = {{"op_type", "Ping"}};
    std::string ping_str = ping_req.dump();
    int64_t op_result = cdd_execute_op(99999, ping_str.c_str(), ping_str.length(), nullptr, 0, nullptr, 0, response_buffer_.data(), response_buffer_.size());
    ASSERT_EQ(op_result, CDD_ERROR_INVALID_HANDLE);
}

// --- Test Group 2: Core Operations (Happy Paths) ---

TEST_F(CApiTest, PingAndClientKey) {
    json mem_config = {{"backend", {{"type", "Memory"}, {"mode", "WriteTruncate"}}}};
    cdd_handle_t handle = create_context(mem_config);
    ASSERT_GT(handle, 0);

    json request = {
        {"op_type", "Ping"},
        {"client_key", "ping-test-123"}
    };
    json result = execute_op(handle, request);

    ASSERT_FALSE(result.is_null());
    ASSERT_EQ(result["message"], "Pong");
    ASSERT_EQ(result["client_key"], "ping-test-123");
    ASSERT_EQ(result["metadata"]["backend_type"], "Memory");
    ASSERT_GT(result["metadata"]["duration_us"].get<int64_t>(), 0);
}

TEST_F(CApiTest, MetadataOperations) {
    // FIX: Use a file backend to persist data between write and read operations.
    test_filepath_ = generate_unique_test_filepath();

    const std::string user_meta_str = "This is my custom metadata!";
    const auto user_meta_bytes = cryptodd::memory::vector<std::byte>(
        reinterpret_cast<const std::byte*>(user_meta_str.data()),
        reinterpret_cast<const std::byte*>(user_meta_str.data()) + user_meta_str.size()
    );
    const std::string user_meta_b64 = cryptodd::ffi::base64::encode(user_meta_bytes);

    // Phase 1: Write the metadata
    {
        json file_write_config = {{"backend", {{"type", "File"}, {"mode", "WriteTruncate"}, {"path", test_filepath_.string()}}}};
        cdd_handle_t writer_handle = create_context(file_write_config);
        ASSERT_GT(writer_handle, 0);

        json set_meta_req = {
            {"op_type", "SetUserMetadata"},
            {"user_metadata_base64", user_meta_b64}
        };
        auto set_meta_res = execute_op(writer_handle, set_meta_req);
        ASSERT_FALSE(set_meta_res.is_null());
        ASSERT_EQ(set_meta_res["status"], "Metadata updated.");

        // Destroy the writer handle to ensure data is flushed and the file is closed.
        handles_to_cleanup_.pop_back();
    }

    // Phase 2: Read and verify the metadata
    {
        json file_read_config = {{"backend", {{"type", "File"}, {"mode", "Read"}, {"path", test_filepath_.string()}}}};
        cdd_handle_t reader_handle = create_context(file_read_config);
        ASSERT_GT(reader_handle, 0);

        json get_meta_req = {{"op_type", "GetUserMetadata"}};
        auto get_meta_res = execute_op(reader_handle, get_meta_req);
        ASSERT_FALSE(get_meta_res.is_null());
        ASSERT_EQ(get_meta_res["user_metadata_base64"], user_meta_b64);
    }
}

// --- Test Group 3: Full Workflow Integration Test ---

TEST_F(CApiTest, FullWorkflowWithFile) {
    test_filepath_ = generate_unique_test_filepath();
    cdd_handle_t writer_handle, reader_handle;

    // === PHASE 1: WRITE ===
    {
        json file_write_config = {{"backend", {{"type", "File"}, {"mode", "WriteTruncate"}, {"path", test_filepath_.string()}}}};
        writer_handle = create_context(file_write_config);
        ASSERT_GT(writer_handle, 0);
        
        // 1. Set User Metadata
        const std::string user_meta_str = "Full workflow metadata";
        const auto user_meta_bytes = cryptodd::memory::vector<std::byte>(
            reinterpret_cast<const std::byte*>(user_meta_str.data()),
            reinterpret_cast<const std::byte*>(user_meta_str.data()) + user_meta_str.size()
        );
        const std::string user_meta_b64 = cryptodd::ffi::base64::encode(user_meta_bytes);
        execute_op(writer_handle, {{"op_type", "SetUserMetadata"}, {"user_metadata_base64", user_meta_b64}});

        // 2. Store a single chunk of uint8_t
        auto data1 = generate_random_data(100); // 10x10
        execute_op(writer_handle, {{"op_type", "StoreChunk"}, {"data_spec", {{"dtype", "UINT8"}, {"shape", {10, 10}}}}, {"encoding", {{"codec", "RAW"}}}}, data1);

        // 3. Store an array of float32, chunked (deterministic data for verification)
        const int num_rows = 100, num_cols = 20;
        std::vector<float> data2_vec(num_rows * num_cols);
        std::iota(data2_vec.begin(), data2_vec.end(), 0.0f); // Deterministic data
        std::span<const std::byte> data2(reinterpret_cast<const std::byte*>(data2_vec.data()), data2_vec.size() * sizeof(float));
        execute_op(writer_handle, {{"op_type", "StoreArray"}, {"data_spec", {{"dtype", "FLOAT32"}, {"shape", {num_rows, num_cols}}}}, {"encoding", {{"codec", "ZSTD_COMPRESSED"}}}, {"chunking_strategy", {{"strategy", "ByCount"}, {"rows_per_chunk", 25}}}}, data2);

        // 4. Store a final chunk of int64_t
        std::vector<int64_t> data3_vec = {1, 2, 3, 4, 5, -1, -2, -3, -4, -5};
        std::span<const std::byte> data3(reinterpret_cast<const std::byte*>(data3_vec.data()), data3_vec.size() * sizeof(int64_t));
        execute_op(writer_handle, {{"op_type", "StoreChunk"}, {"data_spec", {{"dtype", "INT64"}, {"shape", {10}}}}, {"encoding", {{"codec", "RAW"}}}}, data3);

        // 5. Flush and close writer (implicitly done by destroying handle)
        execute_op(writer_handle, {{"op_type", "Flush"}});
        if (auto& h = handles_to_cleanup_.back(); h && *h == writer_handle)
        {
            handles_to_cleanup_.pop_back();
        }
    } // writer_handle is destroyed here

    // === PHASE 2: READ and VERIFY ===
    {
        json file_read_config = {{"backend", {{"type", "File"}, {"mode", "Read"}, {"path", test_filepath_.string()}}}};
        reader_handle = create_context(file_read_config);
        ASSERT_GT(reader_handle, 0);

        // 6. Inspect the file
        auto inspect_res = execute_op(reader_handle, {{"op_type", "Inspect"}});
        ASSERT_FALSE(inspect_res.is_null());
        ASSERT_EQ(inspect_res["total_chunks"], 6);
        ASSERT_EQ(inspect_res["file_header"]["user_metadata_base64"], cryptodd::ffi::base64::encode(std::as_bytes(std::span(std::string_view("Full workflow metadata")))));
        ASSERT_EQ(inspect_res["chunk_summaries"][2]["shape"], json::array({25, 20}));
        ASSERT_EQ(inspect_res["chunk_summaries"][5]["dtype"], "INT64");

        // 7. Load the array chunks (by range) and verify data
        std::vector<std::byte> read_buffer2(100 * 20 * sizeof(float));
        json load_array_req = {{"op_type", "LoadChunks"}, {"selection", {{"type", "Range"}, {"start_index", 1}, {"count", 4}}}};
        auto load_array_res = execute_op(reader_handle, load_array_req, {}, read_buffer2);
        ASSERT_FALSE(load_array_res.is_null());
        ASSERT_EQ(load_array_res["bytes_written_to_output"], 100 * 20 * sizeof(float));
        ASSERT_EQ(load_array_res["final_shape"], json::array({100, 20}));
        // Verify deterministic data content
        auto data2_vec = std::vector<float>(100*20);
        std::iota(data2_vec.begin(), data2_vec.end(), 0.0f);
        ASSERT_EQ(0, std::memcmp(read_buffer2.data(), reinterpret_cast<const std::byte*>(data2_vec.data()), 100*20*sizeof(float)));

        // 8. Load ALL chunks (heterogeneous types)
        size_t total_size = 100 + (100 * 20 * sizeof(float)) + (10 * sizeof(int64_t));
        std::vector<std::byte> read_buffer_all(total_size);
        auto load_all_res = execute_op(reader_handle, {{"op_type", "LoadChunks"}, {"selection", {{"type", "All"}}}}, {}, read_buffer_all);
        ASSERT_FALSE(load_all_res.is_null());
        ASSERT_EQ(load_all_res["bytes_written_to_output"], total_size);
        ASSERT_FALSE(load_all_res.contains("final_shape")); // Dtypes are different, so no shape.
    } // reader_handle is destroyed here
}

// --- Test Group 4: Advanced Codec and Error Handling ---

TEST_F(CApiTest, TemporalOrderbookCodec) {
    test_filepath_ = generate_unique_test_filepath();
    json file_write_config = {{"backend", {{"type", "File"}, {"mode", "WriteTruncate"}, {"path", test_filepath_.string()}}}};
    cdd_handle_t handle = create_context(file_write_config);

    const int N_ROWS = 100, N_FIELDS = 4;
    std::vector<float> orderbook_data;
    orderbook_data.reserve(N_ROWS * N_FIELDS);
    for (int i = 0; i < N_ROWS; ++i) {
        orderbook_data.push_back(1000.0f + i * 0.1f); // bid_price
        orderbook_data.push_back(5.0f + (i % 3 - 1) * 0.1f);   // bid_qty
        orderbook_data.push_back(1000.1f + i * 0.1f); // ask_price
        orderbook_data.push_back(5.0f + (i % 5 - 2) * 0.1f);   // ask_qty
    }
    std::span<const std::byte> input_data(reinterpret_cast<const std::byte*>(orderbook_data.data()), orderbook_data.size() * sizeof(float));

    json store_req = {
        {"op_type", "StoreChunk"},
        {"data_spec", {{"dtype", "FLOAT32"}, {"shape", {N_ROWS, N_FIELDS}}}},
        {"encoding", {{"codec", "TEMPORAL_2D_SIMD_F32"}}}
    };
    auto store_res = execute_op(handle, store_req, input_data);
    ASSERT_LT(store_res["details"]["compressed_size"].get<int64_t>(), input_data.size()); // Verify compression happened

    // Re-open and verify
    cdd_context_destroy(handle); // Close writer
    if (auto& back = handles_to_cleanup_.back(); back != nullptr && *back == handle)
    {
        handles_to_cleanup_.pop_back();
    }

    json file_read_config = {{"backend", {{"type", "File"}, {"mode", "Read"}, {"path", test_filepath_.string()}}}};
    handle = create_context(file_read_config);

    std::vector<std::byte> output_buffer(input_data.size());
    auto load_res = execute_op(handle, {{"op_type", "LoadChunks"}, {"selection", {{"type", "All"}}}}, {}, output_buffer);
    ASSERT_EQ(load_res["bytes_written_to_output"], input_data.size());
    
    auto loaded_floats = reinterpret_cast<const float*>(output_buffer.data());
    for(size_t i = 0; i < orderbook_data.size(); ++i) {
        ASSERT_FLOAT_EQ(loaded_floats[i], orderbook_data[i]);
    }
}

TEST_F(CApiTest, AdvancedErrorHandling) {
    // This handle is for write-only memory tests (Parts 1, 2, 3)
    json mem_config = {{"backend", {{"type", "Memory"}, {"mode", "WriteTruncate"}}}};
    cdd_handle_t mem_writer_handle = create_context(mem_config);

    // 1. Invalid JSON in request string
    std::string bad_req_str = R"({"op_type": "StoreChunk", "data_spec": })"; // Truncated
    int64_t result_code = cdd_execute_op(mem_writer_handle, bad_req_str.c_str(), bad_req_str.length(), nullptr, 0, nullptr, 0, response_buffer_.data(), response_buffer_.size());
    ASSERT_EQ(result_code, CDD_ERROR_INVALID_JSON);

    // 2. Request missing a required key
    json missing_key_req = {{"op_type", "StoreChunk"}, {"encoding", {{"codec", "RAW"}}}}; // Missing data_spec
    result_code = cdd_execute_op(mem_writer_handle, missing_key_req.dump().c_str(), missing_key_req.dump().length(), nullptr, 0, nullptr, 0, response_buffer_.data(), response_buffer_.size());
    ASSERT_EQ(result_code, CDD_ERROR_OPERATION_FAILED);
    json error_response = json::parse(response_buffer_.data());
    ASSERT_EQ(error_response["status"], "Error");
    ASSERT_TRUE(std::string_view(error_response["error"]["message"].get<std::string>()).find("missing required key: 'data_spec'") != std::string::npos);

    // 3. StoreChunk with mismatched data size
    auto data = generate_random_data(50);
    json mismatch_req = {{"op_type", "StoreChunk"}, {"data_spec", {{"dtype", "UINT8"}, {"shape", {100}}}}, {"encoding", {{"codec", "RAW"}}}};
    result_code = cdd_execute_op(mem_writer_handle, mismatch_req.dump().c_str(), mismatch_req.dump().length(), data.data(), data.size(), nullptr, 0, response_buffer_.data(), response_buffer_.size());
    ASSERT_EQ(result_code, CDD_ERROR_OPERATION_FAILED);
    error_response = json::parse(response_buffer_.data());
    ASSERT_TRUE(std::string_view(error_response["error"]["message"].get<std::string>()).find("does not match shape") != std::string::npos);

    // 4. LoadChunks with output buffer too small
    {
        fs::path file_for_test = generate_unique_test_filepath();
        // Write Phase
        {
            json write_config = {{"backend", {{"type", "File"}, {"mode", "WriteTruncate"}, {"path", file_for_test.string()}}}};
            cdd_handle_t writer_handle = create_context(write_config);
            execute_op(writer_handle, {{"op_type", "StoreChunk"}, {"data_spec", {{"dtype", "UINT8"}, {"shape", {100}}}}, {"encoding", {{"codec", "RAW"}}}}, generate_random_data(100));
            handles_to_cleanup_.pop_back(); // Close the writer
        }
        // Read Phase
        {
            json read_config = {{"backend", {{"type", "File"}, {"mode", "Read"}, {"path", file_for_test.string()}}}};
            cdd_handle_t reader_handle = create_context(read_config);
            std::vector<std::byte> small_buffer(50);
            result_code = cdd_execute_op(reader_handle, R"({"op_type":"LoadChunks","selection":{"type":"All"}})", strlen(R"({"op_type":"LoadChunks","selection":{"type":"All"}})"), nullptr, 0, small_buffer.data(), small_buffer.size(), response_buffer_.data(), response_buffer_.size());
            ASSERT_EQ(result_code, CDD_ERROR_OPERATION_FAILED);
            error_response = json::parse(response_buffer_.data());
            ASSERT_TRUE(std::string_view(error_response["error"]["message"].get<std::string>()).find("Output buffer is too small") != std::string::npos);
        }
        handles_to_cleanup_.pop_back(); // Close the writer
        fs::remove(file_for_test);
    }

    // 5. Write operation on Read-only context
    test_filepath_ = generate_unique_test_filepath(); // Use class member so TearDown cleans it up

    // FIX: Create a valid, empty CDD file to open, instead of a zero-byte file.
    {
        json write_config = {{"backend", {{"type", "File"}, {"mode", "WriteTruncate"}, {"path", test_filepath_.string()}}}};
        cdd_handle_t temp_writer = create_context(write_config);
        handles_to_cleanup_.pop_back(); // Destroys the writer, flushing and closing the file.
    }

    {
        json file_read_config = {{"backend", {{"type", "File"}, {"mode", "Read"}, {"path", test_filepath_.string()}}}};
        cdd_handle_t reader_handle = create_context(file_read_config);
        // This handle is now valid because the file is well-formed.
        ASSERT_GT(reader_handle, 0);

        result_code = cdd_execute_op(reader_handle, mismatch_req.dump().c_str(), mismatch_req.dump().length(), generate_random_data(100).data(), 100, nullptr, 0, response_buffer_.data(), response_buffer_.size());
        ASSERT_EQ(result_code, CDD_ERROR_OPERATION_FAILED);
        error_response = json::parse(response_buffer_.data());
        ASSERT_TRUE(std::string_view(error_response["error"]["message"].get<std::string>()).find("not in a writable mode") != std::string::npos);

        handles_to_cleanup_.pop_back(); // Close the reader
    }
}

// --- Test Group 5: Feature-Specific Tests ---

TEST_F(CApiTest, ZstdCompressionLevels) {
    json mem_config = {{"backend", {{"type", "Memory"}, {"mode", "WriteTruncate"}}}};
    cdd_handle_t handle = create_context(mem_config);
    ASSERT_GT(handle, 0);

    // 1. Happy Path: Use a valid, non-default compression level
    auto data = generate_random_data(100);
    json store_req = {
        {"op_type", "StoreChunk"},
        {"data_spec", {{"dtype", "UINT8"}, {"shape", {100}}}},
        {"encoding", {{"codec", "ZSTD_COMPRESSED"}, {"zstd_level", 5}}}
    };
    auto store_res = execute_op(handle, store_req, data);
    ASSERT_FALSE(store_res.is_null());
    ASSERT_EQ(store_res["zstd_level"], 5);

    // 2. Error Path: Use an invalid compression level
    json bad_level_req = {
        {"op_type", "StoreChunk"},
        {"data_spec", {{"dtype", "UINT8"}, {"shape", {100}}}},
        {"encoding", {{"codec", "ZSTD_COMPRESSED"}, {"zstd_level", 99}}}
    };
    int64_t result_code = cdd_execute_op(handle, bad_level_req.dump().c_str(), bad_level_req.dump().length(), data.data(), data.size(), nullptr, 0, response_buffer_.data(), response_buffer_.size());
    ASSERT_LT(result_code, 0);
    json error_response = json::parse(response_buffer_.data());
    ASSERT_TRUE(std::string_view(error_response["error"]["message"].get<std::string>()).find("Invalid zstd compression level") != std::string::npos);
}


TEST_F(CApiTest, LoadChunksChecksumVerification) {
    test_filepath_ = generate_unique_test_filepath();
    auto original_data = generate_random_data(100);

    // 1. Write a file with one raw chunk
    {
        json write_config = {{"backend", {{"type", "File"}, {"mode", "WriteTruncate"}, {"path", test_filepath_.string()}}}};
        cdd_handle_t writer_handle = create_context(write_config);
        execute_op(writer_handle, {{"op_type", "StoreChunk"}, {"data_spec", {{"dtype", "UINT8"}, {"shape", {100}}}}, {"encoding", {{"codec", "RAW"}}}}, original_data);
        handles_to_cleanup_.pop_back(); // Close writer
    }

    // 2. Manually corrupt the file on disk
    {
        // Open the file just to find the chunk offset
        auto reader_res = cryptodd::DataReader::open(test_filepath_);
        ASSERT_TRUE(reader_res.has_value()) << reader_res.error();
        auto chunk_res = reader_res.value()->get_chunk(0);
        ASSERT_TRUE(chunk_res.has_value()) << chunk_res.error();

        // Calculate the offset to the data payload within the file
        size_t header_size =
            sizeof(uint32_t) + // chunk size
            sizeof(uint16_t) + // type
            sizeof(uint16_t) + // dtype
            sizeof(cryptodd::blake3_hash256_t) + // hash
            sizeof(uint64_t) + // flags
            sizeof(uint32_t) + (chunk_res->shape().size() * sizeof(int64_t)) + // shape
            sizeof(uint32_t);  // data size

        // We need the offset of the chunk itself, which is after the file header and index block
        uint64_t chunk_offset = reader_res.value()->get_index_block_offset() + reader_res.value()->get_index_block_size();
        uint64_t corruption_offset = chunk_offset + header_size + 10; // Corrupt the 11th byte

        // Re-open in binary read/write and corrupt one byte
        std::fstream file(test_filepath_, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.is_open());
        file.seekp(corruption_offset);
        char byte_to_flip;
        file.read(&byte_to_flip, 1);
        file.seekp(corruption_offset);
        byte_to_flip = ~byte_to_flip;
        file.write(&byte_to_flip, 1);
        file.close();
    }

    // 3. Attempt to load with checksum verification enabled (should fail)
    {
        json read_config = {{"backend", {{"type", "File"}, {"mode", "Read"}, {"path", test_filepath_.string()}}}};
        cdd_handle_t reader_handle = create_context(read_config);
        std::vector<std::byte> read_buffer(100);
        json load_req = {{"op_type", "LoadChunks"}, {"selection", {{"type", "All"}}}, {"check_checksums", true}};

        int64_t result_code = cdd_execute_op(reader_handle, load_req.dump().c_str(), load_req.dump().length(), nullptr, 0, read_buffer.data(), read_buffer.size(), response_buffer_.data(), response_buffer_.size());
        ASSERT_EQ(result_code, CDD_ERROR_OPERATION_FAILED);
        json error_response = json::parse(response_buffer_.data());
        ASSERT_TRUE(std::string_view(error_response["error"]["message"].get<std::string>()).find("Checksum mismatch") != std::string::npos);
    }

    // 4. Load again with checksum verification disabled (should succeed)
    {
        json read_config = {{"backend", {{"type", "File"}, {"mode", "Read"}, {"path", test_filepath_.string()}}}};
        cdd_handle_t reader_handle = create_context(read_config);
        std::vector<std::byte> read_buffer(100);
        json load_req = {{"op_type", "LoadChunks"}, {"selection", {{"type", "All"}}}, {"check_checksums", false}};
        
        auto load_res = execute_op(reader_handle, load_req, {}, read_buffer);
        ASSERT_FALSE(load_res.is_null());
        ASSERT_EQ(load_res["bytes_written_to_output"], 100);

        // Verify we read the *corrupted* data, not the original
        ASSERT_NE(0, std::memcmp(read_buffer.data(), original_data.data(), 100));
    }
}

TEST_F(CApiTest, SetMetadataAfterWriteFails) {
    test_filepath_ = generate_unique_test_filepath();
    json write_config = {{"backend", {{"type", "File"}, {"mode", "WriteTruncate"}, {"path", test_filepath_.string()}}}};
    cdd_handle_t writer_handle = create_context(write_config);
    ASSERT_GT(writer_handle, 0);

    // 1. Write one chunk to the file.
    execute_op(writer_handle, {{"op_type", "StoreChunk"}, {"data_spec", {{"dtype", "UINT8"}, {"shape", {10}}}}, {"encoding", {{"codec", "RAW"}}}}, generate_random_data(10));

    // 2. Explicitly flush to ensure the write is committed.
    execute_op(writer_handle, {{"op_type", "Flush"}});

    // 3. Now, attempt to set metadata, which should fail because the file is no longer empty.
    json set_meta_req = {
        {"op_type", "SetUserMetadata"},
        {"user_metadata_base64", "dGVzdA=="} // "test"
    };
    int64_t result_code = cdd_execute_op(writer_handle, set_meta_req.dump().c_str(), set_meta_req.dump().length(), nullptr, 0, nullptr, 0, response_buffer_.data(), response_buffer_.size());
    
    // 4. Verify the failure.
    ASSERT_EQ(result_code, CDD_ERROR_OPERATION_FAILED);
    json error_response = json::parse(response_buffer_.data());
    ASSERT_TRUE(std::string_view(error_response["error"]["message"].get<std::string>()).find("metadata can only be set on a new, empty file") != std::string::npos);
}