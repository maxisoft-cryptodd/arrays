#include <gtest/gtest.h>
#include "../src/file_format/cdd_file_format.h"
#include "../src/data_io/data_writer.h"
#include "../src/data_io/data_reader.h"
#include "../src/memory/allocator.h"
#include <filesystem>
// Include the single-header UUID library for unique filenames
#include "../src/codecs/zstd_compressor.h"
#include "../src/storage/memory_backend.h"
#include "../src/file_format/blake3_stream_hasher.h"
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
    memory::vector<int64_t> shape = {32, 32};
    const memory::vector<std::byte> user_meta = {
        std::byte('u'), std::byte('s'), std::byte('e'), std::byte('r'),
        std::byte(' '), std::byte('m'), std::byte('e'), std::byte('t'), std::byte('a')
    };

    // Write
    {
        auto writer_result = DataWriter::create_new(test_filepath_, 10, user_meta); // Capacity 10, user metadata
        ASSERT_TRUE(writer_result.has_value()) << writer_result.error();
        auto writer = std::move(*writer_result);
        
        ZstdCompressor compressor;
        auto compressed_data_res = compressor.compress(original_data);
        ASSERT_TRUE(compressed_data_res.has_value()) << compressed_data_res.error();

        const auto raw_data_hash = calculate_blake3_hash256(original_data);
        Chunk temp_chunk;
        temp_chunk.set_data(std::move(*compressed_data_res));
        auto append_result = writer->append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::ZSTD, shape, temp_chunk, raw_data_hash);

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
    // The reader no longer auto-decompresses, so we expect the original compressed type.
    ASSERT_EQ(read_chunk.type(), ChunkDataType::ZSTD_COMPRESSED);
    ASSERT_EQ(read_chunk.dtype(), DType::UINT8);
    ASSERT_EQ(read_chunk.flags(), ChunkFlags::ZSTD);
    
    auto read_shape_span = read_chunk.get_shape();
    ASSERT_EQ(read_shape_span.size(), shape.size());
    EXPECT_TRUE(std::equal(shape.begin(), shape.end(), read_shape_span.begin()));

    // Manually decompress the data for verification.
    ZstdCompressor decompressor;
    auto decompressed_res = decompressor.decompress(read_chunk.data());
    ASSERT_TRUE(decompressed_res.has_value()) << decompressed_res.error();
    ASSERT_EQ(*decompressed_res, original_data);
}

TEST_F(CddFileTest, WriterRejectsNegativeShape) {
    auto data = generate_random_data(100);
    memory::vector<int64_t> shape = {10, -10};

    auto writer_result = DataWriter::create_new(test_filepath_);
    ASSERT_TRUE(writer_result.has_value()) << writer_result.error();
    auto writer = std::move(*writer_result);

    const auto raw_data_hash = calculate_blake3_hash256(data);
    Chunk temp_chunk;
    temp_chunk.set_data({data.begin(), data.end()});
    auto append_result = writer->append_chunk(ChunkDataType::RAW, DType::UINT8, ChunkFlags::NONE, shape, temp_chunk, raw_data_hash);

    ASSERT_FALSE(append_result.has_value());
    EXPECT_EQ(append_result.error(), "Shape dimensions cannot be negative.");
}

TEST_F(CddFileTest, WriteAndReadMultipleChunksSingleBlock) {
    auto data1 = generate_random_data(512);
    auto data2 = generate_random_data(2048);
    memory::vector<int64_t> shape1 = {16, 32};
    memory::vector<int64_t> shape2 = {64, 32};

    // Write
    {
        auto writer_result = DataWriter::create_new(test_filepath_, 2); // Capacity 2
        ASSERT_TRUE(writer_result.has_value()) << writer_result.error();
        auto writer = std::move(*writer_result);

        ZstdCompressor compressor;
        auto compressed_data1_res = compressor.compress(data1);
        ASSERT_TRUE(compressed_data1_res.has_value()) << compressed_data1_res.error();
        const auto hash1 = calculate_blake3_hash256(data1);
        Chunk chunk1; 
        chunk1.set_data(std::move(*compressed_data1_res));
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::ZSTD, shape1, chunk1, hash1).has_value());

        auto compressed_data2_res = compressor.compress(data2);
        ASSERT_TRUE(compressed_data2_res.has_value()) << compressed_data2_res.error();
        const auto hash2 = calculate_blake3_hash256(data2);
        Chunk chunk2;
        chunk2.set_data(std::move(*compressed_data2_res));
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::ZSTD, shape2, chunk2, hash2).has_value());
        
        ASSERT_TRUE(writer->flush().has_value());
    }

    // Read
    auto reader_result = DataReader::open(test_filepath_);
    ASSERT_TRUE(reader_result.has_value()) << reader_result.error();
    auto reader = std::move(*reader_result);
    ASSERT_EQ(reader->num_chunks(), 2);

    ZstdCompressor decompressor;

    auto chunk1_result = reader->get_chunk(0);
    ASSERT_TRUE(chunk1_result.has_value()) << chunk1_result.error();
    auto decompressed1_res = decompressor.decompress(chunk1_result->data());
    ASSERT_TRUE(decompressed1_res.has_value()) << decompressed1_res.error();
    ASSERT_EQ(*decompressed1_res, data1);
    auto read_shape1_span = chunk1_result->get_shape();
    ASSERT_EQ(read_shape1_span.size(), shape1.size());
    EXPECT_TRUE(std::equal(shape1.begin(), shape1.end(), read_shape1_span.begin()));

    auto chunk2_result = reader->get_chunk(1);
    ASSERT_TRUE(chunk2_result.has_value()) << chunk2_result.error();
    auto decompressed2_res = decompressor.decompress(chunk2_result->data());
    ASSERT_TRUE(decompressed2_res.has_value()) << decompressed2_res.error();
    ASSERT_EQ(*decompressed2_res, data2);
    auto read_shape2_span = chunk2_result->get_shape();
    ASSERT_EQ(read_shape2_span.size(), shape2.size());
    EXPECT_TRUE(std::equal(shape2.begin(), shape2.end(), read_shape2_span.begin()));
}

TEST_F(CddFileTest, WriteAndReadMultipleChunksMultipleBlocks) {
    auto data1 = generate_random_data(512);
    auto data2 = generate_random_data(2048);
    auto data3 = generate_random_data(100);
    memory::vector<int64_t> shape1 = {16, 32};
    memory::vector<int64_t> shape2 = {32, 64};
    memory::vector<int64_t> shape3 = {10, 10};

    // Write
    {
        auto writer_result = DataWriter::create_new(test_filepath_, 1); // Capacity 1, forces new block on second chunk
        ASSERT_TRUE(writer_result.has_value()) << writer_result.error();
        auto writer = std::move(*writer_result);

        ZstdCompressor compressor;
        auto compressed_data1_res = compressor.compress(data1);
        ASSERT_TRUE(compressed_data1_res.has_value()) << compressed_data1_res.error();
        const auto hash1 = calculate_blake3_hash256(data1);
        Chunk chunk1; 
        chunk1.set_data(std::move(*compressed_data1_res));
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::ZSTD, shape1, chunk1, hash1).has_value());

        auto compressed_data2_res = compressor.compress(data2);
        ASSERT_TRUE(compressed_data2_res.has_value()) << compressed_data2_res.error();
        const auto hash2 = calculate_blake3_hash256(data2);
        Chunk chunk2;
        chunk2.set_data(std::move(*compressed_data2_res));
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::ZSTD, shape2, chunk2, hash2).has_value());

        auto compressed_data3_res = compressor.compress(data3);
        ASSERT_TRUE(compressed_data3_res.has_value()) << compressed_data3_res.error();
        const auto hash3 = calculate_blake3_hash256(data3);
        Chunk chunk3;
        chunk3.set_data(std::move(*compressed_data3_res));
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::ZSTD, shape3, chunk3, hash3).has_value());
        
        ASSERT_TRUE(writer->flush().has_value());
    }

    // Read
    auto reader_result = DataReader::open(test_filepath_);
    ASSERT_TRUE(reader_result.has_value()) << reader_result.error();
    auto reader = std::move(*reader_result);
    ASSERT_EQ(reader->num_chunks(), 3);

    ZstdCompressor decompressor;

    auto chunk1_result = reader->get_chunk(0);
    ASSERT_TRUE(chunk1_result.has_value()) << chunk1_result.error();
    auto decompressed1_res = decompressor.decompress(chunk1_result->data());
    ASSERT_TRUE(decompressed1_res.has_value()) << decompressed1_res.error();
    auto read_shape1_span = chunk1_result->get_shape();
    ASSERT_EQ(read_shape1_span.size(), shape1.size());
    EXPECT_TRUE(std::equal(shape1.begin(), shape1.end(), read_shape1_span.begin()));
    ASSERT_EQ(*decompressed1_res, data1);

    auto chunk2_result = reader->get_chunk(1);
    ASSERT_TRUE(chunk2_result.has_value()) << chunk2_result.error();
    auto decompressed2_res = decompressor.decompress(chunk2_result->data());
    ASSERT_TRUE(decompressed2_res.has_value()) << decompressed2_res.error();
    auto read_shape2_span = chunk2_result->get_shape();
    ASSERT_EQ(read_shape2_span.size(), shape2.size());
    EXPECT_TRUE(std::equal(shape2.begin(), shape2.end(), read_shape2_span.begin()));
    ASSERT_EQ(*decompressed2_res, data2);

    auto chunk3_result = reader->get_chunk(2);
    ASSERT_TRUE(chunk3_result.has_value()) << chunk3_result.error();
    auto decompressed3_res = decompressor.decompress(chunk3_result->data());
    ASSERT_TRUE(decompressed3_res.has_value()) << decompressed3_res.error();
    auto read_shape3_span = chunk3_result->get_shape();
    ASSERT_EQ(read_shape3_span.size(), shape3.size());
    EXPECT_TRUE(std::equal(shape3.begin(), shape3.end(), read_shape3_span.begin()));
    ASSERT_EQ(*decompressed3_res, data3);
}

TEST_F(CddFileTest, AppendToExistingFile) {
    auto data1 = generate_random_data(512);
    memory::vector<int64_t> shape1 = {16, 32};

    // Create file with one chunk
    {
        auto writer_result = DataWriter::create_new(test_filepath_, 1); // Capacity 1
        ASSERT_TRUE(writer_result.has_value()) << writer_result.error();
        auto writer = std::move(*writer_result);

        ZstdCompressor compressor;
        auto compressed_data1_res = compressor.compress(data1);
        ASSERT_TRUE(compressed_data1_res.has_value()) << compressed_data1_res.error();
        const auto hash1 = calculate_blake3_hash256(data1);
        Chunk chunk1;
        chunk1.set_data(std::move(*compressed_data1_res));
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::ZSTD, shape1, chunk1, hash1).has_value());
        
        ASSERT_TRUE(writer->flush().has_value());
    }

    // Append a new chunk
    auto data2 = generate_random_data(1024);
    memory::vector<int64_t> shape2 = {32, 32};
    {
        auto writer_result = DataWriter::open_for_append(test_filepath_); // Open for appending
        ASSERT_TRUE(writer_result.has_value()) << writer_result.error();
        auto writer = std::move(*writer_result);

        ZstdCompressor compressor;
        auto compressed_data2_res = compressor.compress(data2);
        ASSERT_TRUE(compressed_data2_res.has_value()) << compressed_data2_res.error();
        const auto hash2 = calculate_blake3_hash256(data2);
        Chunk chunk2;
        chunk2.set_data(std::move(*compressed_data2_res));
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::ZSTD, shape2, chunk2, hash2).has_value());
        
        ASSERT_TRUE(writer->flush().has_value());
    }

    // Read and verify
    auto reader_result = DataReader::open(test_filepath_);
    ASSERT_TRUE(reader_result.has_value()) << reader_result.error();
    auto reader = std::move(*reader_result);
    ASSERT_EQ(reader->num_chunks(), 2);

    ZstdCompressor decompressor;

    auto chunk1_result = reader->get_chunk(0);
    ASSERT_TRUE(chunk1_result.has_value()) << chunk1_result.error();
    auto decompressed1_res = decompressor.decompress(chunk1_result->data());
    ASSERT_TRUE(decompressed1_res.has_value()) << decompressed1_res.error();
    auto read_shape1_span = chunk1_result->get_shape();
    ASSERT_EQ(read_shape1_span.size(), shape1.size());
    EXPECT_TRUE(std::equal(shape1.begin(), shape1.end(), read_shape1_span.begin()));
    ASSERT_EQ(*decompressed1_res, data1);

    auto chunk2_result = reader->get_chunk(1);
    ASSERT_TRUE(chunk2_result.has_value()) << chunk2_result.error();
    auto decompressed2_res = decompressor.decompress(chunk2_result->data());
    ASSERT_TRUE(decompressed2_res.has_value()) << decompressed2_res.error();
    auto read_shape2_span = chunk2_result->get_shape();
    ASSERT_EQ(read_shape2_span.size(), shape2.size());
    EXPECT_TRUE(std::equal(shape2.begin(), shape2.end(), read_shape2_span.begin()));
    ASSERT_EQ(*decompressed2_res, data2);
}

TEST_F(CddFileTest, GetChunkSlice) {
    auto data1 = generate_random_data(100);
    auto data2 = generate_random_data(200);
    auto data3 = generate_random_data(300);
    auto data4 = generate_random_data(400);
    memory::vector<int64_t> shape1 = {10, 10};
    memory::vector<int64_t> shape2 = {10, 20};
    memory::vector<int64_t> shape3 = {15, 20};
    memory::vector<int64_t> shape4 = {20, 20};

    // Write 4 chunks
    {
        auto writer_result = DataWriter::create_new(test_filepath_, 2); // Capacity 2
        ASSERT_TRUE(writer_result.has_value()) << writer_result.error();
        auto writer = std::move(*writer_result);

        ZstdCompressor compressor;
        auto compressed_data1_res = compressor.compress(data1);
        ASSERT_TRUE(compressed_data1_res.has_value()) << compressed_data1_res.error();
        const auto hash1 = calculate_blake3_hash256(data1);
        Chunk chunk1; chunk1.set_data({data1.begin(), data1.end()});
        chunk1.set_data(std::move(*compressed_data1_res));
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::ZSTD, shape1, chunk1, hash1).has_value());

        auto compressed_data2_res = compressor.compress(data2);
        ASSERT_TRUE(compressed_data2_res.has_value()) << compressed_data2_res.error();
        const auto hash2 = calculate_blake3_hash256(data2);
        Chunk chunk2; chunk2.set_data({data2.begin(), data2.end()});
        chunk2.set_data(std::move(*compressed_data2_res));
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::ZSTD, shape2, chunk2, hash2).has_value());

        auto compressed_data3_res = compressor.compress(data3);
        ASSERT_TRUE(compressed_data3_res.has_value()) << compressed_data3_res.error();
        const auto hash3 = calculate_blake3_hash256(data3);
        Chunk chunk3; chunk3.set_data({data3.begin(), data3.end()});
        chunk3.set_data(std::move(*compressed_data3_res));
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::ZSTD, shape3, chunk3, hash3).has_value());

        auto compressed_data4_res = compressor.compress(data4);
        ASSERT_TRUE(compressed_data4_res.has_value()) << compressed_data4_res.error();
        const auto hash4 = calculate_blake3_hash256(data4);
        Chunk chunk4; chunk4.set_data({data4.begin(), data4.end()});
        chunk4.set_data(std::move(*compressed_data4_res));
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::ZSTD, shape4, chunk4, hash4).has_value());
        
        ASSERT_TRUE(writer->flush().has_value());
    }

    auto reader_result = DataReader::open(test_filepath_);
    ASSERT_TRUE(reader_result.has_value()) << reader_result.error();
    auto reader = std::move(*reader_result);
    ASSERT_EQ(reader->num_chunks(), 4);

    ZstdCompressor decompressor;

    // Get slice [0, 1]
    auto slice1_result = reader->get_chunk_slice(0, 1);
    ASSERT_TRUE(slice1_result.has_value()) << slice1_result.error();
    auto& slice1 = *slice1_result;
    ASSERT_EQ(slice1.size(), 2);
    auto decompressed1 = decompressor.decompress(slice1[0]);
    ASSERT_TRUE(decompressed1.has_value()) << decompressed1.error();
    ASSERT_EQ(*decompressed1, data1);
    auto decompressed2 = decompressor.decompress(slice1[1]);
    ASSERT_TRUE(decompressed2.has_value()) << decompressed2.error();
    ASSERT_EQ(*decompressed2, data2);

    // Get slice [1, 3]
    auto slice2_result = reader->get_chunk_slice(1, 3);
    ASSERT_TRUE(slice2_result.has_value()) << slice2_result.error();
    auto& slice2 = *slice2_result;
    ASSERT_EQ(slice2.size(), 3);
    auto decompressed2b = decompressor.decompress(slice2[0]);
    ASSERT_TRUE(decompressed2b.has_value()) << decompressed2b.error();
    ASSERT_EQ(*decompressed2b, data2);
    auto decompressed3 = decompressor.decompress(slice2[1]);
    ASSERT_TRUE(decompressed3.has_value()) << decompressed3.error();
    ASSERT_EQ(*decompressed3, data3);
    auto decompressed4 = decompressor.decompress(slice2[2]);
    ASSERT_TRUE(decompressed4.has_value()) << decompressed4.error();
    ASSERT_EQ(*decompressed4, data4);

    // Get slice [2, 2]
    auto slice3_result = reader->get_chunk_slice(2, 2);
    ASSERT_TRUE(slice3_result.has_value()) << slice3_result.error();
    auto& slice3 = *slice3_result;
    ASSERT_EQ(slice3.size(), 1);
    auto decompressed3b = decompressor.decompress(slice3[0]);
    ASSERT_TRUE(decompressed3b.has_value()) << decompressed3b.error();
    ASSERT_EQ(*decompressed3b, data3);
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
    memory::vector<int64_t> shape = {10, 10};

    std::unique_ptr<storage::IStorageBackend> mem_backend;

    // Write to the in-memory backend
    {
        auto writer_result = DataWriter::create_in_memory(128, {});
        ASSERT_TRUE(writer_result.has_value()) << writer_result.error();
        auto writer = std::move(*writer_result);

        const auto hash1 = calculate_blake3_hash256(data1);
        Chunk chunk1; chunk1.set_data({data1.begin(), data1.end()});
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::RAW, DType::UINT8, ChunkFlags::NONE, shape, chunk1, hash1).has_value());

        const auto hash2 = calculate_blake3_hash256(data2);
        Chunk chunk2; chunk2.set_data({data2.begin(), data2.end()});
        ASSERT_TRUE(writer->append_chunk(ChunkDataType::RAW, DType::UINT8, ChunkFlags::NONE, shape, chunk2, hash2).has_value());
        
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
                vector<int64_t> shape = {static_cast<int64_t>(data.size())};
                
                const auto raw_data_hash = calculate_blake3_hash256(data);
                Chunk temp_chunk;
                temp_chunk.set_data({data.begin(), data.end()});
                ASSERT_TRUE(writer->append_chunk(ChunkDataType::RAW, DType::UINT8, ChunkFlags::NONE, shape, temp_chunk, raw_data_hash).has_value());
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
                vector<int64_t> shape = {static_cast<int64_t>(data.size())};

                const auto raw_data_hash = calculate_blake3_hash256(data);
                Chunk temp_chunk;
                temp_chunk.set_data({data.begin(), data.end()});
                ASSERT_TRUE(writer->append_chunk(ChunkDataType::RAW, DType::UINT8, ChunkFlags::NONE, shape, temp_chunk, raw_data_hash).has_value());
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
