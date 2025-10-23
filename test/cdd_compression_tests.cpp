#include <gtest/gtest.h>
#include "../src/file_format/cdd_file_format.h"
#include "../src/data_io/data_writer.h"
#include "../src/data_io/data_reader.h"
#include "../src/storage/file_backend.h"
#include "../src/file_format/serialization_helpers.h"
#include "test_helpers.h"

#include <filesystem>

namespace fs = std::filesystem;
using namespace cryptodd;

class CddCompressionTest : public ::testing::Test {
protected:
    fs::path test_filepath_;

    void SetUp() override {
        test_filepath_ = generate_unique_test_filepath();
    }

    void TearDown() override {
        try {
            if (fs::exists(test_filepath_)) {
                fs::remove(test_filepath_);
            }
        } catch (const fs::filesystem_error& e) {
            std::cerr << "Warning: Could not clean up test file " << test_filepath_ << ": " << e.what() << std::endl;
        }
    }

    // Helper to append a number of simple, compressible chunks
    void append_chunks(DataWriter& writer, size_t num_chunks) {
        for (size_t i = 0; i < num_chunks; ++i) {
            // Use compressible data
            memory::vector<std::byte> data(256, static_cast<std::byte>(i));
            memory::vector<int64_t> shape = {16, 16};
            const auto raw_data_hash = calculate_blake3_hash256(data);
            Chunk temp_chunk;
            temp_chunk.set_data(std::move(data));
            auto append_result = writer.append_chunk(ChunkDataType::RAW, DType::UINT8, ChunkFlags::NONE, shape, temp_chunk, raw_data_hash);
            ASSERT_TRUE(append_result.has_value()) << append_result.error();
        }
    }
};

TEST_F(CddCompressionTest, CompressionSuccess) {
    const size_t capacity = 256;
    const size_t total_chunks = capacity + 1;
    memory::vector<memory::vector<std::byte>> original_chunks;

    // --- WRITE ---
    {
        auto writer_result = DataWriter::create_new(test_filepath_, capacity);
        ASSERT_TRUE(writer_result.has_value()) << writer_result.error();
        auto writer = std::move(*writer_result);

        for (size_t i = 0; i < total_chunks; ++i) {
            memory::vector<std::byte> data(256, static_cast<std::byte>(i));
            original_chunks.push_back(data);
            memory::vector<int64_t> shape = {16, 16};
            const auto raw_data_hash = calculate_blake3_hash256(data);
            Chunk temp_chunk;
            temp_chunk.set_data(std::move(data));
            auto append_result = writer->append_chunk(ChunkDataType::RAW, DType::UINT8, ChunkFlags::NONE, shape, temp_chunk, raw_data_hash);
            ASSERT_TRUE(append_result.has_value()) << append_result.error();
        }
        ASSERT_TRUE(writer->flush().has_value());
    }

    // --- MANUAL VERIFICATION of block type ---
    {
        storage::FileBackend backend(test_filepath_, std::ios_base::in | std::ios_base::binary);
        FileHeader header;
        ASSERT_TRUE(header.read(backend).has_value());

        // We are now at the start of the first ChunkOffsetsBlock.
        // Read its header to check the type.
        auto size_res = serialization::read_pod<uint32_t>(backend);
        ASSERT_TRUE(size_res.has_value()) << size_res.error();

        auto type_res = serialization::read_pod<ChunkOffsetType>(backend);
        ASSERT_TRUE(type_res.has_value()) << type_res.error();
        EXPECT_EQ(*type_res, ChunkOffsetType::ZSTD_COMPRESSED);
    }

    // --- READ and VERIFY all data ---
    {
        auto reader_result = DataReader::open(test_filepath_);
        ASSERT_TRUE(reader_result.has_value()) << reader_result.error();
        auto reader = std::move(*reader_result);
        ASSERT_EQ(reader->num_chunks(), total_chunks);

        for (size_t i = 0; i < total_chunks; ++i) {
            auto chunk_result = reader->get_chunk(i);
            ASSERT_TRUE(chunk_result.has_value()) << "Failed to get chunk " << i << ": " << chunk_result.error();
            EXPECT_EQ(chunk_result->data(), original_chunks[i]) << "Data mismatch for chunk " << i;
        }
    }
}

TEST_F(CddCompressionTest, CompressionFallbackForIncompressibleData) {
    const size_t capacity = 1;
    const size_t total_chunks = capacity + 1;
    memory::vector<memory::vector<std::byte>> original_chunks;

    // --- WRITE ---
    {
        auto writer_result = DataWriter::create_new(test_filepath_, capacity);
        ASSERT_TRUE(writer_result.has_value()) << writer_result.error();
        auto writer = std::move(*writer_result);
        writer->set_compression_level(22); // Force fallback

        for (size_t i = 0; i < total_chunks; ++i) {
            // Use random, incompressible data
            memory::vector<std::byte> data = generate_random_data(256);
            original_chunks.push_back(data);
            memory::vector<int64_t> shape = {16, 16};
            const auto raw_data_hash = calculate_blake3_hash256(data);
            Chunk temp_chunk;
            temp_chunk.set_data(std::move(data));
            auto append_result = writer->append_chunk(ChunkDataType::RAW, DType::UINT8, ChunkFlags::NONE, shape, temp_chunk, raw_data_hash);
            ASSERT_TRUE(append_result.has_value()) << append_result.error();
        }
        ASSERT_TRUE(writer->flush().has_value());
    }

    // --- MANUAL VERIFICATION of block type ---
    {
        storage::FileBackend backend(test_filepath_, std::ios_base::in | std::ios_base::binary);
        FileHeader header;
        ASSERT_TRUE(header.read(backend).has_value());

        // We are now at the start of the first ChunkOffsetsBlock.
        // Read its header to check the type.
        auto size_res = serialization::read_pod<uint32_t>(backend);
        ASSERT_TRUE(size_res.has_value()) << size_res.error();

        auto type_res = serialization::read_pod<ChunkOffsetType>(backend);
        ASSERT_TRUE(type_res.has_value()) << type_res.error();
        EXPECT_EQ(*type_res, ChunkOffsetType::RAW);
    }

    // --- READ and VERIFY all data ---
    {
        auto reader_result = DataReader::open(test_filepath_);
        ASSERT_TRUE(reader_result.has_value()) << reader_result.error();
        auto reader = std::move(*reader_result);
        ASSERT_EQ(reader->num_chunks(), total_chunks);

        for (size_t i = 0; i < total_chunks; ++i) {
            auto chunk_result = reader->get_chunk(i);
            ASSERT_TRUE(chunk_result.has_value()) << "Failed to get chunk " << i << ": " << chunk_result.error();
            EXPECT_EQ(chunk_result->data(), original_chunks[i]) << "Data mismatch for chunk " << i;
        }
    }
}

TEST_F(CddCompressionTest, PaddingIntegrity) {
    const size_t capacity = 256;
    const size_t total_chunks = capacity + 1;

    // --- WRITE ---
    {
        auto writer_result = DataWriter::create_new(test_filepath_, capacity);
        ASSERT_TRUE(writer_result.has_value()) << writer_result.error();
        auto writer = std::move(*writer_result);

        for (size_t i = 0; i < total_chunks; ++i) {
            memory::vector<std::byte> data(256, static_cast<std::byte>(0)); // Highly compressible
            memory::vector<int64_t> shape = {16, 16};
            const auto raw_data_hash = calculate_blake3_hash256(data);
            Chunk temp_chunk;
            temp_chunk.set_data(std::move(data));
            auto append_result = writer->append_chunk(ChunkDataType::RAW, DType::UINT8, ChunkFlags::NONE, shape, temp_chunk, raw_data_hash);
            ASSERT_TRUE(append_result.has_value()) << append_result.error();
        }
        ASSERT_TRUE(writer->flush().has_value());
    }

    // --- MANUAL VERIFICATION of padding ---
    {
        storage::FileBackend backend(test_filepath_, std::ios_base::in | std::ios_base::binary);
        FileHeader header;
        ASSERT_TRUE(header.read(backend).has_value());

        // --- First Block ---
        auto block1_start_offset_res = backend.tell();
        ASSERT_TRUE(block1_start_offset_res.has_value());

        auto size_res = serialization::read_pod<uint32_t>(backend);
        ASSERT_TRUE(size_res.has_value()) << size_res.error();
        const uint32_t block_size_on_disk = *size_res;

        auto type_res = serialization::read_pod<ChunkOffsetType>(backend);
        ASSERT_TRUE(type_res.has_value()) << type_res.error();
        ASSERT_EQ(*type_res, ChunkOffsetType::ZSTD_COMPRESSED);

        auto hash_res = serialization::read_pod<blake3_hash256_t>(backend);
        ASSERT_TRUE(hash_res.has_value()) << hash_res.error();

        auto next_ptr_res = serialization::read_pod<uint64_t>(backend);
        ASSERT_TRUE(next_ptr_res.has_value()) << next_ptr_res.error();

        auto compressed_size_res = serialization::read_pod<uint32_t>(backend);
        ASSERT_TRUE(compressed_size_res.has_value()) << compressed_size_res.error();
        const uint32_t compressed_size = *compressed_size_res;

        // Seek past compressed data
        auto pos_after_blob_size_res = backend.tell();
        ASSERT_TRUE(pos_after_blob_size_res.has_value());
        ASSERT_TRUE(backend.seek(*pos_after_blob_size_res + compressed_size).has_value());

        // Verify padding
        auto padding_start_offset_res = backend.tell();
        ASSERT_TRUE(padding_start_offset_res.has_value());

        const uint64_t next_block_start_offset = *block1_start_offset_res + block_size_on_disk;
        const size_t padding_size = next_block_start_offset - *padding_start_offset_res;

        if (padding_size > 0) {
            memory::vector<std::byte> padding_data(padding_size);
            auto read_res = backend.read(padding_data);
            ASSERT_TRUE(read_res.has_value()) << read_res.error();
            ASSERT_EQ(*read_res, padding_size);

            const memory::vector<std::byte> expected_zeros(padding_size, std::byte{0});
            EXPECT_EQ(padding_data, expected_zeros);
        }

        // --- Second Block ---
        // Verify we are at the start of the next block
        auto second_block_start_res = backend.tell();
        ASSERT_TRUE(second_block_start_res.has_value());
        EXPECT_EQ(*second_block_start_res, next_block_start_offset);

        // Try to read its header
        auto second_size_res = serialization::read_pod<uint32_t>(backend);
        ASSERT_TRUE(second_size_res.has_value()) << second_size_res.error();
        auto second_type_res = serialization::read_pod<ChunkOffsetType>(backend);
        ASSERT_TRUE(second_type_res.has_value()) << second_type_res.error();
        EXPECT_EQ(*second_type_res, ChunkOffsetType::RAW);
    }
}

TEST_F(CddCompressionTest, MetadataIntegrityAfterCompression) {
    const size_t capacity = 4;
    const size_t total_chunks = capacity + 1;
    const memory::vector<std::byte> user_meta = {
        std::byte('m'), std::byte('e'), std::byte('t'), std::byte('a'), std::byte('d'), std::byte('a'), std::byte('t'), std::byte('a')
    };

    // --- WRITE ---
    {
        auto writer_result = DataWriter::create_new(test_filepath_, capacity, user_meta);
        ASSERT_TRUE(writer_result.has_value()) << writer_result.error();
        auto writer = std::move(*writer_result);

        for (size_t i = 0; i < total_chunks; ++i) {
            memory::vector<std::byte> data(256, static_cast<std::byte>(i));
            memory::vector<int64_t> shape = {16, 16};
            const auto raw_data_hash = calculate_blake3_hash256(data);
            Chunk temp_chunk;
            temp_chunk.set_data(std::move(data));
            auto append_result = writer->append_chunk(ChunkDataType::RAW, DType::UINT8, ChunkFlags::NONE, shape, temp_chunk, raw_data_hash);
            ASSERT_TRUE(append_result.has_value()) << append_result.error();
        }
        ASSERT_TRUE(writer->flush().has_value());
    }

    // --- READ and VERIFY ---
    {
        auto reader_result = DataReader::open(test_filepath_);
        ASSERT_TRUE(reader_result.has_value()) << reader_result.error();
        auto reader = std::move(*reader_result);
        ASSERT_EQ(reader->num_chunks(), total_chunks);

        // Verify user metadata
        EXPECT_TRUE(UserMetadataMatches(*reader, user_meta));
    }
}

TEST_F(CddCompressionTest, AppendStressTest) {
    const size_t capacity = 128;
    const size_t total_chunks = 4096;
    memory::vector<memory::vector<std::byte>> original_chunks;

    // --- WRITE ---
    {
        auto writer_result = DataWriter::create_new(test_filepath_, capacity);
        ASSERT_TRUE(writer_result.has_value()) << writer_result.error();
        auto writer = std::move(*writer_result);

        for (size_t i = 0; i < total_chunks; ++i) {
            // Alternate between compressible and incompressible data
            memory::vector<std::byte> data = (i % 2 == 0)
                ? memory::vector<std::byte>(128, static_cast<std::byte>(i))
                : generate_random_data(128);

            original_chunks.push_back(data);
            memory::vector<int64_t> shape = {128};
            const auto raw_data_hash = calculate_blake3_hash256(data);
            Chunk temp_chunk;
            temp_chunk.set_data(std::move(data));
            auto append_result = writer->append_chunk(ChunkDataType::RAW, DType::UINT8, ChunkFlags::NONE, shape, temp_chunk, raw_data_hash);
            ASSERT_TRUE(append_result.has_value()) << append_result.error();
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
            ASSERT_TRUE(chunk_result.has_value()) << "Failed to get chunk " << i << ": " << chunk_result.error();
            EXPECT_EQ(chunk_result->data(), original_chunks[i]) << "Data mismatch for chunk " << i;
        }
    }
}
