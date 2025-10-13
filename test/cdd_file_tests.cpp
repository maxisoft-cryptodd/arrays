#include <gtest/gtest.h>
#include "../src/file_format/cdd_file_format.h"
#include "../src/data_io/data_writer.h"
#include "../src/data_io/data_reader.h"
#include <filesystem>
// Include the single-header UUID library for unique filenames
#include "../src/codecs/zstd_compressor.h"
#include "../src/storage/memory_backend.h"
#include "test_helpers.h"

namespace fs = std::filesystem;
using namespace cryptodd;

// Custom predicate assertion for verifying user metadata.
// This encapsulates the decompression logic and provides a clear failure message.
::testing::AssertionResult UserMetadataMatches(const DataReader& reader, std::span<const std::byte> expected_meta) {
    const auto& compressed_meta = reader.get_file_header().user_metadata();
    if (compressed_meta.empty() && expected_meta.empty()) {
        return ::testing::AssertionSuccess();
    }

    // Use a local compressor instance to decompress the metadata for verification.
    ZstdCompressor compressor;
    auto decompressed_result = compressor.decompress(compressed_meta);
    if (!decompressed_result) {
        return ::testing::AssertionFailure() << "Failed to decompress user metadata: " << decompressed_result.error();
    }

    if (std::equal(decompressed_result->begin(), decompressed_result->end(), expected_meta.begin(), expected_meta.end())) {
        return ::testing::AssertionSuccess();
    }

    // This conversion to string is only for readable test output and assumes ASCII-compatible content.
    return ::testing::AssertionFailure() << "User metadata does not match.\n"
           << "      Expected: " << std::string(reinterpret_cast<const char*>(expected_meta.data()), expected_meta.size()) << "\n"
           << "        Actual: " << std::string(reinterpret_cast<const char*>(decompressed_result->data()), decompressed_result->size());
}

// Test fixture for file-based tests
class CddFileTest : public ::testing::Test {
protected:
    // Each test will get a unique temporary file path.
    fs::path test_filepath_;

    void SetUp() override {
        test_filepath_ = generate_unique_test_filepath();
    }

    void TearDown() override {
        // Clean up test file
        // Use a try-catch block as filesystem operations can fail.
        try {
            if (fs::exists(test_filepath_)) {
                fs::remove(test_filepath_);
            }
        } catch (const fs::filesystem_error& e) {
            // Log the error but don't fail the test during cleanup.
            std::cerr << "Warning: Could not clean up test file " << test_filepath_ << ": " << e.what() << std::endl;
        }
    }
};

TEST_F(CddFileTest, WriteAndReadEmptyFile) {
    // Create a new file
    {
        auto writer_result = DataWriter::create_new(test_filepath_);
        ASSERT_TRUE(writer_result.has_value()) << writer_result.error();
        auto writer = std::move(*writer_result);
        auto flush_result = writer->flush(); // Ensure header is written
        ASSERT_TRUE(flush_result.has_value()) << flush_result.error();
    }

    // Read the file
    auto reader_result = DataReader::open(test_filepath_);
    ASSERT_TRUE(reader_result.has_value()) << reader_result.error();
    auto reader = std::move(*reader_result);
    ASSERT_EQ(reader->num_chunks(), 0);
}

TEST_F(CddFileTest, WriteAndReadSingleChunk) {
    auto original_data = generate_random_data(1024);
    memory::vector<uint32_t> shape = {32, 32, 0}; // 32x32 array, null-terminated
    const memory::vector<std::byte> user_meta = {
        std::byte('u'), std::byte('s'), std::byte('e'), std::byte('r'),
        std::byte(' '), std::byte('m'), std::byte('e'), std::byte('t'), std::byte('a')
    };

    // Write
    {
        auto writer_result = DataWriter::create_new(test_filepath_, 10, user_meta); // Capacity 10, user metadata
        ASSERT_TRUE(writer_result.has_value()) << writer_result.error();
        auto writer = std::move(*writer_result);
        auto append_result = writer->append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape, original_data);
        ASSERT_TRUE(append_result.has_value()) << append_result.error();
        auto flush_result = writer->flush();
        ASSERT_TRUE(flush_result.has_value()) << flush_result.error();
    }

    // Read
    auto reader_result = DataReader::open(test_filepath_);
    ASSERT_TRUE(reader_result.has_value()) << reader_result.error();
    auto reader = std::move(*reader_result);
    ASSERT_EQ(reader->num_chunks(), 1);

    // Verify user metadata using a custom predicate for better error messages.
    EXPECT_TRUE(UserMetadataMatches(*reader, user_meta));

    auto chunk_result = reader->get_chunk(0);
    ASSERT_TRUE(chunk_result.has_value()) << chunk_result.error();
    auto& read_chunk = *chunk_result;
    ASSERT_EQ(read_chunk.type(), ChunkDataType::RAW); // Should be RAW after decompression
    ASSERT_EQ(read_chunk.dtype(), DType::UINT8);
    ASSERT_EQ(read_chunk.flags(), ChunkFlags::NONE);
    ASSERT_EQ(read_chunk.shape(), shape);
    ASSERT_EQ(read_chunk.data(), original_data);
}

TEST_F(CddFileTest, WriteAndReadMultipleChunksSingleBlock) {
    auto data1 = generate_random_data(512);
    auto data2 = generate_random_data(2048);
    memory::vector<uint32_t> shape1 = {16, 32, 0};
    memory::vector<uint32_t> shape2 = {64, 32, 0};

    // Write
    {
        auto writer_result = DataWriter::create_new(test_filepath_, 2); // Capacity 2
        ASSERT_TRUE(writer_result.has_value()) << writer_result.error();
        auto writer = std::move(*writer_result);
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape1, data1).has_value());
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape2, data2).has_value());
        ASSERT_TRUE(writer->flush().has_value());
    }

    // Read
    auto reader_result = DataReader::open(test_filepath_);
    ASSERT_TRUE(reader_result.has_value()) << reader_result.error();
    auto reader = std::move(*reader_result);
    ASSERT_EQ(reader->num_chunks(), 2);

    auto chunk1_result = reader->get_chunk(0);
    ASSERT_TRUE(chunk1_result.has_value()) << chunk1_result.error();
    ASSERT_EQ(chunk1_result->data(), data1);
    ASSERT_EQ(chunk1_result->shape(), shape1);

    auto chunk2_result = reader->get_chunk(1);
    ASSERT_TRUE(chunk2_result.has_value()) << chunk2_result.error();
    ASSERT_EQ(chunk2_result->data(), data2);
    ASSERT_EQ(chunk2_result->shape(), shape2);
}

TEST_F(CddFileTest, WriteAndReadMultipleChunksMultipleBlocks) {
    auto data1 = generate_random_data(512);
    auto data2 = generate_random_data(2048);
    auto data3 = generate_random_data(100);
    memory::vector<uint32_t> shape1 = {16, 32, 0}; // 16*32 = 512
    memory::vector<uint32_t> shape2 = {32, 64, 0}; // 32*64 = 2048
    memory::vector<uint32_t> shape3 = {10, 10, 0}; // 10*10 = 100

    // Write
    {
        auto writer_result = DataWriter::create_new(test_filepath_, 1); // Capacity 1, forces new block on second chunk
        ASSERT_TRUE(writer_result.has_value()) << writer_result.error();
        auto writer = std::move(*writer_result);
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape1, data1).has_value());
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape2, data2).has_value());
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape3, data3).has_value());
        ASSERT_TRUE(writer->flush().has_value());
    }

    // Read
    auto reader_result = DataReader::open(test_filepath_);
    ASSERT_TRUE(reader_result.has_value()) << reader_result.error();
    auto reader = std::move(*reader_result);
    ASSERT_EQ(reader->num_chunks(), 3);

    auto chunk1_result = reader->get_chunk(0);
    ASSERT_TRUE(chunk1_result.has_value()) << chunk1_result.error();
    ASSERT_EQ(chunk1_result->shape(), shape1);
    ASSERT_EQ(chunk1_result->data(), data1);

    auto chunk2_result = reader->get_chunk(1);
    ASSERT_TRUE(chunk2_result.has_value()) << chunk2_result.error();
    ASSERT_EQ(chunk2_result->shape(), shape2);
    ASSERT_EQ(chunk2_result->data(), data2);

    auto chunk3_result = reader->get_chunk(2);
    ASSERT_TRUE(chunk3_result.has_value()) << chunk3_result.error();
    ASSERT_EQ(chunk3_result->shape(), shape3);
    ASSERT_EQ(chunk3_result->data(), data3);
}

TEST_F(CddFileTest, AppendToExistingFile) {
    auto data1 = generate_random_data(512);
    memory::vector<uint32_t> shape1 = {16, 32, 0}; // 16*32 = 512

    // Create file with one chunk
    {
        auto writer_result = DataWriter::create_new(test_filepath_, 1); // Capacity 1
        ASSERT_TRUE(writer_result.has_value()) << writer_result.error();
        auto writer = std::move(*writer_result);
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape1, data1).has_value());
        ASSERT_TRUE(writer->flush().has_value());
    }

    // Append a new chunk
    auto data2 = generate_random_data(1024);
    memory::vector<uint32_t> shape2 = {32, 32, 0}; // 32*32 = 1024
    {
        auto writer_result = DataWriter::open_for_append(test_filepath_); // Open for appending
        ASSERT_TRUE(writer_result.has_value()) << writer_result.error();
        auto writer = std::move(*writer_result);
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape2, data2).has_value());
        ASSERT_TRUE(writer->flush().has_value());
    }

    // Read and verify
    auto reader_result = DataReader::open(test_filepath_);
    ASSERT_TRUE(reader_result.has_value()) << reader_result.error();
    auto reader = std::move(*reader_result);
    ASSERT_EQ(reader->num_chunks(), 2);

    auto chunk1_result = reader->get_chunk(0);
    ASSERT_TRUE(chunk1_result.has_value()) << chunk1_result.error();
    ASSERT_EQ(chunk1_result->shape(), shape1);
    ASSERT_EQ(chunk1_result->data(), data1);

    auto chunk2_result = reader->get_chunk(1);
    ASSERT_TRUE(chunk2_result.has_value()) << chunk2_result.error();
    ASSERT_EQ(chunk2_result->shape(), shape2);
    ASSERT_EQ(chunk2_result->data(), data2);
}

TEST_F(CddFileTest, GetChunkSlice) {
    auto data1 = generate_random_data(100);
    auto data2 = generate_random_data(200);
    auto data3 = generate_random_data(300);
    auto data4 = generate_random_data(400);
    memory::vector<uint32_t> shape1 = {10, 10, 0};   // 100 bytes
    memory::vector<uint32_t> shape2 = {10, 20, 0};   // 200 bytes
    memory::vector<uint32_t> shape3 = {15, 20, 0};   // 300 bytes
    memory::vector<uint32_t> shape4 = {20, 20, 0};   // 400 bytes

    // Write 4 chunks
    {
        auto writer_result = DataWriter::create_new(test_filepath_, 2); // Capacity 2
        ASSERT_TRUE(writer_result.has_value()) << writer_result.error();
        auto writer = std::move(*writer_result);
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape1, data1).has_value());
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape2, data2).has_value());
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape3, data3).has_value());
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape4, data4).has_value());
        ASSERT_TRUE(writer->flush().has_value());
    }

    auto reader_result = DataReader::open(test_filepath_);
    ASSERT_TRUE(reader_result.has_value()) << reader_result.error();
    auto reader = std::move(*reader_result);
    ASSERT_EQ(reader->num_chunks(), 4);

    // Get slice [0, 1]
    auto slice1_result = reader->get_chunk_slice(0, 1);
    ASSERT_TRUE(slice1_result.has_value()) << slice1_result.error();
    auto& slice1 = *slice1_result;
    ASSERT_EQ(slice1.size(), 2);
    ASSERT_EQ(slice1[0], data1);
    ASSERT_EQ(slice1[1], data2);

    // Get slice [1, 3]
    auto slice2_result = reader->get_chunk_slice(1, 3);
    ASSERT_TRUE(slice2_result.has_value()) << slice2_result.error();
    auto& slice2 = *slice2_result;
    ASSERT_EQ(slice2.size(), 3);
    ASSERT_EQ(slice2[0], data2);
    ASSERT_EQ(slice2[1], data3);
    ASSERT_EQ(slice2[2], data4);

    // Get slice [2, 2]
    auto slice3_result = reader->get_chunk_slice(2, 2);
    ASSERT_TRUE(slice3_result.has_value()) << slice3_result.error();
    auto& slice3 = *slice3_result;
    ASSERT_EQ(slice3.size(), 1);
    ASSERT_EQ(slice3[0], data3);
}

TEST_F(CddFileTest, MemoryBackendTest) {
    auto original_data = generate_random_data(1024);

    storage::MemoryBackend mem_backend;

    ASSERT_TRUE(mem_backend.seek(0).has_value());
    auto write_res = mem_backend.write(original_data);
    ASSERT_TRUE(write_res.has_value()) << write_res.error();
    auto size_res = mem_backend.size();
    ASSERT_TRUE(size_res.has_value()) << size_res.error();
    ASSERT_EQ(*size_res, 1024);

    memory::vector<std::byte> read_data(1024);
    ASSERT_TRUE(mem_backend.seek(0).has_value());
    auto read_res = mem_backend.read(read_data);
    ASSERT_TRUE(read_res.has_value()) << read_res.error();
    ASSERT_EQ(read_data, original_data);

    ASSERT_TRUE(mem_backend.seek(512).has_value());
    auto tell_res = mem_backend.tell();
    ASSERT_TRUE(tell_res.has_value()) << tell_res.error();
    ASSERT_EQ(*tell_res, 512);

    auto partial_data = generate_random_data(100);
    ASSERT_TRUE(mem_backend.seek(512).has_value());
    ASSERT_TRUE(mem_backend.write(partial_data).has_value());
    size_res = mem_backend.size();
    ASSERT_TRUE(size_res.has_value()) << size_res.error();
    ASSERT_EQ(*size_res, 1024); // Size should not change if overwriting within bounds

    auto extended_data = generate_random_data(200);
    ASSERT_TRUE(mem_backend.seek(1100).has_value());
    ASSERT_TRUE(mem_backend.write(extended_data).has_value());
    size_res = mem_backend.size();
    ASSERT_TRUE(size_res.has_value()) << size_res.error();
    ASSERT_EQ(*size_res, 1300); // Size should extend
}

TEST_F(CddFileTest, InMemoryWriterToReader) {
    auto data1 = generate_random_data(100);
    auto data2 = generate_random_data(200);
    memory::vector<uint32_t> shape = {10, 10, 0};

    std::unique_ptr<storage::IStorageBackend> mem_backend;

    // Write to the in-memory backend
    {
        auto writer_result = DataWriter::create_in_memory(128, {});
        ASSERT_TRUE(writer_result.has_value()) << writer_result.error();
        auto writer = std::move(*writer_result);
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::RAW, DType::UINT8, ChunkFlags::NONE, shape, data1).has_value());
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::RAW, DType::UINT8, ChunkFlags::NONE, shape, data2).has_value());
        ASSERT_TRUE(writer->flush().has_value());
        auto backend_result = writer->release_backend(); // Retrieve the backend from the writer
        ASSERT_TRUE(backend_result.has_value()) << backend_result.error();
        mem_backend = std::move(*backend_result);
    }

    // Manually rewind the backend before passing it to the reader. This is the caller's responsibility.
    ASSERT_TRUE(mem_backend->rewind().has_value());

    // Read from the same in-memory backend
    auto reader_result = DataReader::open_in_memory(std::move(mem_backend));
    ASSERT_TRUE(reader_result.has_value()) << reader_result.error();
    auto reader = std::move(*reader_result);
    ASSERT_EQ(reader->num_chunks(), 2);

    auto chunk1_result = reader->get_chunk(0);
    ASSERT_TRUE(chunk1_result.has_value()) << chunk1_result.error();
    ASSERT_EQ(chunk1_result->data(), data1);

    auto chunk2_result = reader->get_chunk(1);
    ASSERT_TRUE(chunk2_result.has_value()) << chunk2_result.error();
    ASSERT_EQ(chunk2_result->data(), data2);
}

// --- Parameterized Test for Chunk Offset Block Chaining ---

struct ChunkOffsetChainingTestParams {
    size_t capacity;
    size_t num_extra_chunks;
};

// This test fixture is parameterized to test the dynamic creation of new
// ChunkOffsetsBlocks when the current one is full.
class CddChunkOffsetChainingTest : public CddFileTest,
                                   public ::testing::WithParamInterface<ChunkOffsetChainingTestParams> {
};

TEST_P(CddChunkOffsetChainingTest, HandlesDynamicBlockAllocation) {

    using cryptodd::memory::vector;
    const auto& params = GetParam();
    const size_t capacity = params.capacity;
    const size_t num_extra_chunks = params.num_extra_chunks;
    const size_t total_chunks = capacity + num_extra_chunks;

    vector<vector<std::byte>> original_data_chunks;
    original_data_chunks.reserve(total_chunks);

    // --- Test with FileBackend ---
    {
        // --- WRITE ---
        {
            auto writer_result = DataWriter::create_new(test_filepath_, capacity);
            ASSERT_TRUE(writer_result.has_value()) << writer_result.error();
            auto writer = std::move(*writer_result);
            for (size_t i = 0; i < total_chunks; ++i) {
                auto data = generate_random_data(10 + i); // Varying sizes
                original_data_chunks.push_back(data);
                vector<uint32_t> shape = {static_cast<uint32_t>(data.size()), 0};
                ASSERT_TRUE(writer->append_chunk(ChunkDataType::RAW, DType::UINT8, ChunkFlags::NONE, shape, data).has_value());
            }
            ASSERT_TRUE(writer->flush().has_value());
        }

        // --- READ and VERIFY ---
        {
            auto reader_result = DataReader::open(test_filepath_);
            ASSERT_TRUE(reader_result.has_value()) << reader_result.error();
            auto reader = std::move(*reader_result);
            ASSERT_EQ(reader->num_chunks(), total_chunks);
            for (size_t i = 0; i < total_chunks; ++i) {
                auto chunk_result = reader->get_chunk(i);
                ASSERT_TRUE(chunk_result.has_value()) << chunk_result.error();
                ASSERT_EQ(chunk_result->data(), original_data_chunks[i]) << "FileBackend: Mismatch at chunk " << i;
            }
        }
    }

    // --- Test with MemoryBackend ---
    original_data_chunks.clear(); // Reset for memory test
    {
        std::unique_ptr<storage::IStorageBackend> mem_backend;
        // --- WRITE ---
        {
            auto writer_result = DataWriter::create_in_memory(capacity);
            ASSERT_TRUE(writer_result.has_value()) << writer_result.error();
            auto writer = std::move(*writer_result);
            for (size_t i = 0; i < total_chunks; ++i) {
                auto data = generate_random_data(10 + i);
                original_data_chunks.push_back(data);
                vector<uint32_t> shape = {static_cast<uint32_t>(data.size()), 0};
                ASSERT_TRUE(writer->append_chunk(ChunkDataType::RAW, DType::UINT8, ChunkFlags::NONE, shape, data).has_value());
            }
            ASSERT_TRUE(writer->flush().has_value());
            auto backend_result = writer->release_backend();
            ASSERT_TRUE(backend_result.has_value()) << backend_result.error();
            mem_backend = std::move(*backend_result);
        }

        // --- READ and VERIFY ---
        {
            ASSERT_TRUE(mem_backend->rewind().has_value());
            auto reader_result = DataReader::open_in_memory(std::move(mem_backend));
            ASSERT_TRUE(reader_result.has_value()) << reader_result.error();
            auto reader = std::move(*reader_result);
            ASSERT_EQ(reader->num_chunks(), total_chunks);
            for (size_t i = 0; i < total_chunks; ++i) {
                auto chunk_result = reader->get_chunk(i);
                ASSERT_TRUE(chunk_result.has_value()) << chunk_result.error();
                ASSERT_EQ(chunk_result->data(), original_data_chunks[i]) << "MemoryBackend: Mismatch at chunk " << i;
            }
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    ChunkOffsetChaining,
    CddChunkOffsetChainingTest,
    ::testing::Values(
        ChunkOffsetChainingTestParams{2, 1}, // Fill block of 2, add 1 more
        ChunkOffsetChainingTestParams{2, 2}, // Fill block of 2, add 2 more (fill a second block)
        ChunkOffsetChainingTestParams{3, 3}, // Fill block of 3, add 3 more (fill a second block)
        ChunkOffsetChainingTestParams{4, 5}  // Fill block of 4, add 5 more (span a third block)
    ),
    [](const ::testing::TestParamInfo<CddChunkOffsetChainingTest::ParamType>& info) {
        return "Capacity" + std::to_string(info.param.capacity) + "_Append" + std::to_string(info.param.num_extra_chunks) + "Extra";
    }
);
