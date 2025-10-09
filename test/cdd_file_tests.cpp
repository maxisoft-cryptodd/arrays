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

// Test fixture for file-based tests
class CddFileTest : public ::testing::Test {
protected:
    // Each test will get a unique temporary file path.
    fs::path test_filepath_;
    static inline int test_counter_ = 0; // C++17 inline static member for unique naming

    void SetUp() override {
        // Generate a unique filename in the system's temporary directory for each test.
        test_filepath_ = fs::temp_directory_path() / ("cryptodd_test_" + std::to_string(test_counter_++) + ".cdd");
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
    writer.flush();

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

    // Verify user metadata
    size_t bound = ZSTD_getFrameContentSize(reader.get_file_header().user_metadata.data(), reader.get_file_header().user_metadata.size());
    ASSERT_NE(bound, ZSTD_CONTENTSIZE_ERROR);
    std::vector<uint8_t> decompressed_user_meta = DataReader::decompress_zstd(reader.get_file_header().user_metadata, bound);
    ASSERT_EQ(decompressed_user_meta, user_meta);


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
    std::vector<uint32_t> shape = {10, 10, 0};

    // Write
    DataWriter writer = DataWriter::create_new(test_filepath_, 1); // Capacity 1, forces new block on second chunk
    writer.append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape, data1);
    writer.append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape, data2);
    writer.append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape, data3);
    writer.flush();

    // Read
    DataReader reader = DataReader::open(test_filepath_);
    ASSERT_EQ(reader.num_chunks(), 3);

    Chunk read_chunk1 = reader.get_chunk(0);
    ASSERT_EQ(read_chunk1.data, data1);

    Chunk read_chunk2 = reader.get_chunk(1);
    ASSERT_EQ(read_chunk2.data, data2);

    Chunk read_chunk3 = reader.get_chunk(2);
    ASSERT_EQ(read_chunk3.data, data3);
}

TEST_F(CddFileTest, AppendToExistingFile) {
    std::vector<uint8_t> data1 = generate_random_data(512);
    std::vector<uint32_t> shape = {10, 10, 0};

    // Create file with one chunk
    {
        DataWriter writer = DataWriter::create_new(test_filepath_, 1); // Capacity 1
        writer.append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape, data1);
        writer.flush();
    }

    // Append a new chunk
    std::vector<uint8_t> data2 = generate_random_data(1024);
    {
        DataWriter writer = DataWriter::open_for_append(test_filepath_); // Open for appending
        writer.append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape, data2);
        writer.flush();
    }

    // Read and verify
    DataReader reader = DataReader::open(test_filepath_);
    ASSERT_EQ(reader.num_chunks(), 2);
    ASSERT_EQ(reader.get_chunk(0).data, data1);
    ASSERT_EQ(reader.get_chunk(1).data, data2);
}

TEST_F(CddFileTest, GetChunkSlice) {
    std::vector<uint8_t> data1 = generate_random_data(100);
    std::vector<uint8_t> data2 = generate_random_data(200);
    std::vector<uint8_t> data3 = generate_random_data(300);
    std::vector<uint8_t> data4 = generate_random_data(400);
    std::vector<uint32_t> shape = {10, 10, 0};

    // Write 4 chunks
    DataWriter writer = DataWriter::create_new(test_filepath_, 2); // Capacity 2
    writer.append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape, data1);
    writer.append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape, data2);
    writer.append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape, data3);
    writer.append_chunk(ChunkDataType::ZSTD_COMPRESSED, DType::UINT8, ChunkFlags::NONE, shape, data4);
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
    std::vector<uint32_t> shape = {32, 32, 0};

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
