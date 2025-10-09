// Prevent problematic windows.h macros on Windows
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#endif

#include <gtest/gtest.h>
#include "../src/storage/storage_backend.h"
#include "../src/file_format/cdd_file_format.h"
#include "../src/data_io/data_writer.h"
#include "../src/data_io/data_reader.h"
#include <filesystem>
#include <random>
// Include the single-header UUID library for unique filenames
#include <stduuid/uuid.h>

namespace fs = std::filesystem;
using namespace cryptodd;

// Helper to generate random data
std::vector<uint8_t> generate_random_data(size_t size) {
    // Use a static generator for better performance and to avoid re-seeding on every call.
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> distrib(0, 255);

    std::vector<uint8_t> data(size);
    std::ranges::generate(data, []() {
        return static_cast<uint8_t>(distrib(gen));
    });
    return data;
}

// Helper to create a unique temporary filepath for tests.
fs::path generate_unique_test_filepath() {
    // Generate a truly unique filename using a UUID to prevent test collisions.
    // This is robust even for parallel test execution.
    std::random_device rd;
    auto seed_data = std::array<int, std::mt19937::state_size>{};
    std::ranges::generate(seed_data, std::ref(rd));
    std::seed_seq seq(std::begin(seed_data), std::end(seed_data));
    std::mt19937 generator(seq);
    uuids::uuid_random_generator gen{generator};

    const uuids::uuid id = gen();
    const std::string filename = "cryptodd_test_" + uuids::to_string(id) + ".cdd";
    return fs::temp_directory_path() / filename;
}

// Custom predicate assertion for verifying user metadata.
// This encapsulates the decompression logic and provides a clear failure message.
::testing::AssertionResult UserMetadataMatches(const DataReader& reader, const std::vector<uint8_t>& expected_meta) {
    const auto& compressed_meta = reader.get_file_header().user_metadata;
    std::vector<uint8_t> decompressed_meta;
    try {
        size_t bound = ZSTD_getFrameContentSize(compressed_meta.data(), compressed_meta.size());
        if (bound == ZSTD_CONTENTSIZE_ERROR) {
            return ::testing::AssertionFailure() << "User metadata is not a valid Zstd frame.";
        }
        decompressed_meta = DataReader::decompress_zstd(compressed_meta, bound);
    } catch (const std::exception& e) {
        return ::testing::AssertionFailure() << "Failed to decompress user metadata: " << e.what();
    }

    if (decompressed_meta == expected_meta) {
        return ::testing::AssertionSuccess();
    }

    return ::testing::AssertionFailure()
           << "User metadata does not match.\n"
           << "      Expected: " << std::string(expected_meta.begin(), expected_meta.end()) << "\n"
           << "        Actual: " << std::string(decompressed_meta.begin(), decompressed_meta.end());
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
    DataWriter writer = DataWriter::create_new(test_filepath_);
    writer.flush(); // Ensure header is written

    // Read the file
    const DataReader reader = DataReader::open(test_filepath_);
    ASSERT_EQ(reader.num_chunks(), 0);
}

TEST_F(CddFileTest, WriteAndReadSingleChunk) {
    std::vector<uint8_t> original_data = generate_random_data(1024);
    std::vector<uint32_t> shape = {32, 32, 0}; // 32x32 array, null-terminated
    std::vector<uint8_t> user_meta = {'u', 's', 'e', 'r', ' ', 'm', 'e', 't', 'a'};

    // Write
    DataWriter writer = DataWriter::create_new(test_filepath_, 10, user_meta); // Capacity 10, user metadata
    writer.append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape, original_data);
    writer.flush();

    // Read
    DataReader reader = DataReader::open(test_filepath_);
    ASSERT_EQ(reader.num_chunks(), 1);

    // Verify user metadata using a custom predicate for better error messages.
    EXPECT_TRUE(UserMetadataMatches(reader, user_meta));

    Chunk read_chunk = reader.get_chunk(0);
    ASSERT_EQ(read_chunk.type, ChunkDataType::RAW); // Should be RAW after decompression
    ASSERT_EQ(read_chunk.dtype, DType::UINT8);
    ASSERT_EQ(read_chunk.flags, ChunkFlags::NONE);
    ASSERT_EQ(read_chunk.shape, shape);
    ASSERT_EQ(read_chunk.data, original_data);
}

TEST_F(CddFileTest, WriteAndReadMultipleChunksSingleBlock) {
    std::vector<uint8_t> data1 = generate_random_data(512);
    std::vector<uint8_t> data2 = generate_random_data(2048);
    std::vector<uint32_t> shape1 = {16, 32, 0};
    std::vector<uint32_t> shape2 = {64, 32, 0};

    // Write
    DataWriter writer = DataWriter::create_new(test_filepath_, 2); // Capacity 2
    writer.append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape1, data1);
    writer.append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape2, data2);
    writer.flush();

    // Read
    DataReader reader = DataReader::open(test_filepath_);
    ASSERT_EQ(reader.num_chunks(), 2);

    Chunk read_chunk1 = reader.get_chunk(0);
    ASSERT_EQ(read_chunk1.data, data1);
    ASSERT_EQ(read_chunk1.shape, shape1);

    Chunk read_chunk2 = reader.get_chunk(1);
    ASSERT_EQ(read_chunk2.data, data2);
    ASSERT_EQ(read_chunk2.shape, shape2);
}

TEST_F(CddFileTest, WriteAndReadMultipleChunksMultipleBlocks) {
    std::vector<uint8_t> data1 = generate_random_data(512);
    std::vector<uint8_t> data2 = generate_random_data(2048);
    std::vector<uint8_t> data3 = generate_random_data(100);
    std::vector<uint32_t> shape1 = {16, 32, 0}; // 16*32 = 512
    std::vector<uint32_t> shape2 = {32, 64, 0}; // 32*64 = 2048
    std::vector<uint32_t> shape3 = {10, 10, 0}; // 10*10 = 100

    // Write
    DataWriter writer = DataWriter::create_new(test_filepath_, 1); // Capacity 1, forces new block on second chunk
    writer.append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape1, data1);
    writer.append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape2, data2);
    writer.append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape3, data3);
    writer.flush();

    // Read
    DataReader reader = DataReader::open(test_filepath_);
    ASSERT_EQ(reader.num_chunks(), 3);

    Chunk read_chunk1 = reader.get_chunk(0);
    ASSERT_EQ(read_chunk1.shape, shape1);
    ASSERT_EQ(read_chunk1.data, data1);

    Chunk read_chunk2 = reader.get_chunk(1);
    ASSERT_EQ(read_chunk2.shape, shape2);
    ASSERT_EQ(read_chunk2.data, data2);

    Chunk read_chunk3 = reader.get_chunk(2);
    ASSERT_EQ(read_chunk3.shape, shape3);
    ASSERT_EQ(read_chunk3.data, data3);
}

TEST_F(CddFileTest, AppendToExistingFile) {
    std::vector<uint8_t> data1 = generate_random_data(512);
    std::vector<uint32_t> shape1 = {16, 32, 0}; // 16*32 = 512

    // Create file with one chunk
    {
        DataWriter writer = DataWriter::create_new(test_filepath_, 1); // Capacity 1
        writer.append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape1, data1);
        writer.flush();
    }

    // Append a new chunk
    std::vector<uint8_t> data2 = generate_random_data(1024);
    std::vector<uint32_t> shape2 = {32, 32, 0}; // 32*32 = 1024
    {
        DataWriter writer = DataWriter::open_for_append(test_filepath_); // Open for appending
        writer.append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape2, data2);
        writer.flush();
    }

    // Read and verify
    DataReader reader = DataReader::open(test_filepath_);
    ASSERT_EQ(reader.num_chunks(), 2);
    Chunk chunk1 = reader.get_chunk(0);
    ASSERT_EQ(chunk1.shape, shape1);
    ASSERT_EQ(chunk1.data, data1);
    Chunk chunk2 = reader.get_chunk(1);
    ASSERT_EQ(chunk2.shape, shape2);
    ASSERT_EQ(chunk2.data, data2);
}

TEST_F(CddFileTest, GetChunkSlice) {
    std::vector<uint8_t> data1 = generate_random_data(100);
    std::vector<uint8_t> data2 = generate_random_data(200);
    std::vector<uint8_t> data3 = generate_random_data(300);
    std::vector<uint8_t> data4 = generate_random_data(400);
    std::vector<uint32_t> shape1 = {10, 10, 0};   // 100 bytes
    std::vector<uint32_t> shape2 = {10, 20, 0};   // 200 bytes
    std::vector<uint32_t> shape3 = {15, 20, 0};   // 300 bytes
    std::vector<uint32_t> shape4 = {20, 20, 0};   // 400 bytes

    // Write 4 chunks
    DataWriter writer = DataWriter::create_new(test_filepath_, 2); // Capacity 2
    writer.append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape1, data1);
    writer.append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape2, data2);
    writer.append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape3, data3);
    writer.append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape4, data4);
    writer.flush();

    DataReader reader = DataReader::open(test_filepath_);
    ASSERT_EQ(reader.num_chunks(), 4);

    // Get slice [0, 1]
    std::vector<std::vector<uint8_t>> slice1 = reader.get_chunk_slice(0, 1);
    ASSERT_EQ(slice1.size(), 2);
    ASSERT_EQ(slice1[0], data1);
    ASSERT_EQ(slice1[1], data2);

    // Get slice [1, 3]
    std::vector<std::vector<uint8_t>> slice2 = reader.get_chunk_slice(1, 3);
    ASSERT_EQ(slice2.size(), 3);
    ASSERT_EQ(slice2[0], data2);
    ASSERT_EQ(slice2[1], data3);
    ASSERT_EQ(slice2[2], data4);

    // Get slice [2, 2]
    std::vector<std::vector<uint8_t>> slice3 = reader.get_chunk_slice(2, 2);
    ASSERT_EQ(slice3.size(), 1);
    ASSERT_EQ(slice3[0], data3);
}

TEST_F(CddFileTest, MemoryBackendTest) {
    std::vector<uint8_t> original_data = generate_random_data(1024);

    MemoryBackend mem_backend;
    // For in-memory, we need to manually construct DataWriter to use MemoryBackend
    // This requires modifying DataWriter to accept a backend directly, or creating a test-specific writer.
    // For now, let's just test MemoryBackend directly.

    mem_backend.seek(0);
    mem_backend.write(original_data);
    ASSERT_EQ(mem_backend.size(), 1024);

    std::vector<uint8_t> read_data(1024);
    mem_backend.seek(0);
    mem_backend.read(read_data);
    ASSERT_EQ(read_data, original_data);

    mem_backend.seek(512);
    ASSERT_EQ(mem_backend.tell(), 512);

    std::vector<uint8_t> partial_data = generate_random_data(100);
    mem_backend.seek(512);
    mem_backend.write(partial_data);
    ASSERT_EQ(mem_backend.size(), 1024); // Size should not change if overwriting within bounds

    std::vector<uint8_t> extended_data = generate_random_data(200);
    mem_backend.seek(1100);
    mem_backend.write(extended_data);
    ASSERT_EQ(mem_backend.size(), 1300); // Size should extend
}

TEST_F(CddFileTest, InMemoryWriterToReader) {
    std::vector<uint8_t> data1 = generate_random_data(100);
    std::vector<uint8_t> data2 = generate_random_data(200);
    std::vector<uint32_t> shape = {10, 10, 0};

    // The DataWriter takes ownership of the backend, so we can't share it directly.
    // We create a writer, let it go out of scope, and then create a reader with the same backend.
    std::unique_ptr<IStorageBackend> mem_backend;

    // Write to the in-memory backend
    {
        DataWriter writer = DataWriter::create_in_memory(128, {});
        writer.append_chunk(ChunkDataType::RAW, DType::UINT8, ChunkFlags::NONE, shape, data1);
        writer.append_chunk(ChunkDataType::RAW, DType::UINT8, ChunkFlags::NONE, shape, data2);
        writer.flush();
        mem_backend = std::move(writer.release_backend()); // Retrieve the backend from the writer
    }

    // Manually rewind the backend before passing it to the reader. This is the caller's responsibility.
    mem_backend->rewind();

    // Read from the same in-memory backend
    DataReader reader = DataReader::open_in_memory(std::move(mem_backend));
    ASSERT_EQ(reader.num_chunks(), 2);
    ASSERT_EQ(reader.get_chunk(0).data, data1);
    ASSERT_EQ(reader.get_chunk(1).data, data2);
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
    const auto& params = GetParam();
    const size_t capacity = params.capacity;
    const size_t num_extra_chunks = params.num_extra_chunks;
    const size_t total_chunks = capacity + num_extra_chunks;

    std::vector<std::vector<uint8_t>> original_data_chunks;
    original_data_chunks.reserve(total_chunks);

    // --- Test with FileBackend ---
    {
        // --- WRITE ---
        {
            DataWriter writer = DataWriter::create_new(test_filepath_, capacity);
            for (size_t i = 0; i < total_chunks; ++i) {
                std::vector<uint8_t> data = generate_random_data(10 + i); // Varying sizes
                original_data_chunks.push_back(data);
                std::vector<uint32_t> shape = {static_cast<uint32_t>(data.size()), 0};
                writer.append_chunk(ChunkDataType::RAW, DType::UINT8, ChunkFlags::NONE, shape, data);
            }
            writer.flush();
        }

        // --- READ and VERIFY ---
        {
            DataReader reader = DataReader::open(test_filepath_);
            ASSERT_EQ(reader.num_chunks(), total_chunks);
            for (size_t i = 0; i < total_chunks; ++i) {
                ASSERT_EQ(reader.get_chunk(i).data, original_data_chunks[i]) << "FileBackend: Mismatch at chunk " << i;
            }
        }
    }

    // --- Test with MemoryBackend ---
    original_data_chunks.clear(); // Reset for memory test
    {
        std::unique_ptr<IStorageBackend> mem_backend;
        // --- WRITE ---
        {
            DataWriter writer = DataWriter::create_in_memory(capacity);
            for (size_t i = 0; i < total_chunks; ++i) {
                std::vector<uint8_t> data = generate_random_data(10 + i);
                original_data_chunks.push_back(data);
                std::vector<uint32_t> shape = {static_cast<uint32_t>(data.size()), 0};
                writer.append_chunk(ChunkDataType::RAW, DType::UINT8, ChunkFlags::NONE, shape, data);
            }
            writer.flush();
            mem_backend = writer.release_backend();
        }

        // --- READ and VERIFY ---
        {
            mem_backend->rewind();
            DataReader reader = DataReader::open_in_memory(std::move(mem_backend));
            ASSERT_EQ(reader.num_chunks(), total_chunks);
            for (size_t i = 0; i < total_chunks; ++i) {
                ASSERT_EQ(reader.get_chunk(i).data, original_data_chunks[i]) << "MemoryBackend: Mismatch at chunk " << i;
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
