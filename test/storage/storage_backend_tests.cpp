#include <gtest/gtest.h>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include <algorithm>

#include "i_storage_backend.h"
#include "file_backend.h"
#include "memory_backend.h"
#include "mio_backend.h"
#include "../test_helpers.h"

using namespace cryptodd::storage;
namespace fs = std::filesystem;


class StorageBackendTest : public ::testing::TestWithParam<std::string> {
protected:
    std::unique_ptr<IStorageBackend> backend_;
    fs::path test_filepath_;

    void SetUp() override {
        const auto& backend_type = GetParam();
        if (backend_type == "MemoryBackend") {
            backend_ = std::make_unique<MemoryBackend>();
        } else {
            test_filepath_ = generate_unique_test_filepath();
            if (backend_type == "FileBackend") {
                backend_ = std::make_unique<FileBackend>(test_filepath_);
            } else if (backend_type == "MioBackend") {
                backend_ = std::make_unique<MioBackend>(test_filepath_);
            }
        }
        ASSERT_TRUE(backend_ != nullptr);
    }

    void TearDown() override {
        // Release the backend to close file handles before trying to delete.
        backend_.reset();
        if (!test_filepath_.empty() && fs::exists(test_filepath_)) {
            try {
                fs::remove(test_filepath_);
            } catch (const fs::filesystem_error& e) {
                std::cerr << "Warning: Could not clean up test file " << test_filepath_ << ": " << e.what() << std::endl;
            }
        }
    }
};

TEST_P(StorageBackendTest, InitialStateIsEmpty) {
    auto size_res = backend_->size();
    ASSERT_TRUE(size_res.has_value()) << size_res.error();
    EXPECT_EQ(*size_res, 0);

    auto tell_res = backend_->tell();
    ASSERT_TRUE(tell_res.has_value()) << tell_res.error();
    EXPECT_EQ(*tell_res, 0);
}

TEST_P(StorageBackendTest, SimpleWriteAndRead) {
    auto original_data = generate_random_data(128);

    auto write_res = backend_->write(original_data);
    ASSERT_TRUE(write_res.has_value()) << write_res.error();
    EXPECT_EQ(*write_res, original_data.size());

    auto size_res = backend_->size();
    ASSERT_TRUE(size_res.has_value()) << size_res.error();
    EXPECT_EQ(*size_res, original_data.size());

    auto tell_res = backend_->tell();
    ASSERT_TRUE(tell_res.has_value()) << tell_res.error();
    EXPECT_EQ(*tell_res, original_data.size());

    ASSERT_TRUE(backend_->rewind().has_value());

    cryptodd::memory::vector<std::byte> read_data(original_data.size());
    auto read_res = backend_->read(read_data);
    ASSERT_TRUE(read_res.has_value()) << read_res.error();
    EXPECT_EQ(*read_res, original_data.size());

    EXPECT_EQ(read_data, original_data);
}

TEST_P(StorageBackendTest, Overwrite) {
    auto initial_data = generate_random_data(256);
    ASSERT_TRUE(backend_->write(initial_data).has_value());

    auto overwrite_data = generate_random_data(64);
    const uint64_t overwrite_offset = 100;
    ASSERT_TRUE(backend_->seek(overwrite_offset).has_value());

    auto write_res = backend_->write(overwrite_data);
    ASSERT_TRUE(write_res.has_value()) << write_res.error();
    EXPECT_EQ(*write_res, overwrite_data.size());

    auto size_res = backend_->size();
    ASSERT_TRUE(size_res.has_value()) << size_res.error();
    EXPECT_EQ(*size_res, initial_data.size());

    // Construct the expected final state
    cryptodd::memory::vector<std::byte> expected_data = initial_data;
    std::copy(overwrite_data.begin(), overwrite_data.end(), expected_data.begin() + overwrite_offset);

    // Read back the entire content and verify
    cryptodd::memory::vector<std::byte> actual_data(expected_data.size());
    ASSERT_TRUE(backend_->rewind().has_value());
    auto read_res = backend_->read(actual_data);
    ASSERT_TRUE(read_res.has_value()) << read_res.error();
    EXPECT_EQ(*read_res, expected_data.size());
    EXPECT_EQ(actual_data, expected_data);
}

TEST_P(StorageBackendTest, WritePastEnd) {
    auto initial_data = generate_random_data(100);
    ASSERT_TRUE(backend_->write(initial_data).has_value());

    const uint64_t seek_pos = 200;
    ASSERT_TRUE(backend_->seek(seek_pos).has_value());

    auto append_data = generate_random_data(50);
    auto write_res = backend_->write(append_data);
    ASSERT_TRUE(write_res.has_value()) << write_res.error();
    EXPECT_EQ(*write_res, append_data.size());

    const uint64_t expected_size = seek_pos + append_data.size();
    auto size_res = backend_->size();
    ASSERT_TRUE(size_res.has_value()) << size_res.error();
    EXPECT_EQ(*size_res, expected_size);

    // Read back the appended data
    cryptodd::memory::vector<std::byte> read_back_data(append_data.size());
    ASSERT_TRUE(backend_->seek(seek_pos).has_value());
    auto read_res = backend_->read(read_back_data);
    ASSERT_TRUE(read_res.has_value()) << read_res.error();
    EXPECT_EQ(*read_res, append_data.size());
    EXPECT_EQ(read_back_data, append_data);

    // Check that the gap is zero-filled (for file-based backends)
    if (GetParam() != "MemoryBackend") {
        cryptodd::memory::vector<std::byte> gap_data(10);
        ASSERT_TRUE(backend_->seek(150).has_value());
        ASSERT_TRUE(backend_->read(gap_data).has_value());
        for(const auto& b : gap_data) {
            EXPECT_EQ(b, std::byte{0});
        }
    }
}

TEST_P(StorageBackendTest, ReadPastEnd) {
    auto data = generate_random_data(50);
    ASSERT_TRUE(backend_->write(data).has_value());

    ASSERT_TRUE(backend_->seek(50).has_value());
    std::vector<std::byte> buffer(10);
    auto read_res = backend_->read(buffer);
    ASSERT_TRUE(read_res.has_value()) << read_res.error();
    EXPECT_EQ(*read_res, 0); // Should read 0 bytes at EOF
}

TEST_P(StorageBackendTest, ReadOnlyMode) {
    if (GetParam() == "MemoryBackend") {
        // MemoryBackend doesn't have a read-only mode in its constructor
        GTEST_SKIP();
    }

    auto data = generate_random_data(100);
    {
        // First, create and write to the file
        std::unique_ptr<IStorageBackend> writer_backend;
        if (GetParam() == "FileBackend") {
            writer_backend = std::make_unique<FileBackend>(test_filepath_);
        } else {
            writer_backend = std::make_unique<MioBackend>(test_filepath_);
        }
        ASSERT_TRUE(writer_backend->write(data).has_value());
        ASSERT_TRUE(writer_backend->flush().has_value());
    }

    // Now, open it in read-only mode
    std::unique_ptr<IStorageBackend> reader_backend;
    if (GetParam() == "FileBackend") {
        reader_backend = std::make_unique<FileBackend>(test_filepath_, std::ios_base::in | std::ios_base::binary);
    } else {
        reader_backend = std::make_unique<MioBackend>(test_filepath_, std::ios_base::in | std::ios_base::binary);
    }

    // Reading should succeed
    cryptodd::memory::vector<std::byte> read_data(data.size());
    auto read_res = reader_backend->read(read_data);
    ASSERT_TRUE(read_res.has_value()) << read_res.error();
    EXPECT_EQ(*read_res, data.size());
    EXPECT_EQ(read_data, data);

    // Writing should fail
    ASSERT_TRUE(reader_backend->rewind().has_value());
    auto write_res = reader_backend->write(data);
    EXPECT_FALSE(write_res.has_value());
}

INSTANTIATE_TEST_SUITE_P(
    AllBackends,
    StorageBackendTest,
    ::testing::Values("MemoryBackend", "FileBackend", "MioBackend"),
    [](const ::testing::TestParamInfo<StorageBackendTest::ParamType>& info) {
        return info.param;
    }
);

// An "adversarial" test to ensure MioBackend behaves identically to MemoryBackend
TEST(MioBackendAdversarialTest, MatchesMemoryBackend) {
    fs::path test_filepath = generate_unique_test_filepath();
    
    auto mio_backend = std::make_unique<MioBackend>(test_filepath);
    auto mem_backend = std::make_unique<MemoryBackend>();

    std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<int> op_dist(0, 2); // 0: write, 1: seek, 2: overwrite
    std::uniform_int_distribution<size_t> size_dist(1, 2048);
    std::uniform_int_distribution<uint64_t> seek_dist(0, 50000);

    const int num_operations = 100;

    for (int i = 0; i < num_operations; ++i) {
        int op = op_dist(gen);
        
        if (op == 0) { // Write
            auto data = generate_random_data(size_dist(gen));
            ASSERT_TRUE(mio_backend->write(data).has_value());
            ASSERT_TRUE(mem_backend->write(data).has_value());
        } else if (op == 1) { // Seek
            uint64_t offset = seek_dist(gen);
            ASSERT_TRUE(mio_backend->seek(offset).has_value());
            ASSERT_TRUE(mem_backend->seek(offset).has_value());
        } else { // Overwrite
            auto current_size_res = mem_backend->size();
            ASSERT_TRUE(current_size_res.has_value());
            if (*current_size_res > 0) {
                std::uniform_int_distribution<uint64_t> overwrite_seek_dist(0, *current_size_res - 1);
                uint64_t offset = overwrite_seek_dist(gen);
                ASSERT_TRUE(mio_backend->seek(offset).has_value());
                ASSERT_TRUE(mem_backend->seek(offset).has_value());
                auto data = generate_random_data(size_dist(gen));
                ASSERT_TRUE(mio_backend->write(data).has_value());
                ASSERT_TRUE(mem_backend->write(data).has_value());
            }
        }

        auto mio_tell = mio_backend->tell();
        auto mem_tell = mem_backend->tell();
        ASSERT_TRUE(mio_tell.has_value());
        ASSERT_TRUE(mem_tell.has_value());
        ASSERT_EQ(*mio_tell, *mem_tell);

        auto mio_size = mio_backend->size();
        auto mem_size = mem_backend->size();
        ASSERT_TRUE(mio_size.has_value());
        ASSERT_TRUE(mem_size.has_value());
        ASSERT_EQ(*mio_size, *mem_size);
    }

    // Final verification: read back all data and compare
    auto final_size_res = mem_backend->size();
    ASSERT_TRUE(final_size_res.has_value());
    uint64_t final_size = *final_size_res;

    if (final_size > 0) {
        std::vector<std::byte> mio_data(final_size);
        std::vector<std::byte> mem_data(final_size);

        ASSERT_TRUE(mio_backend->rewind().has_value());
        ASSERT_TRUE(mem_backend->rewind().has_value());

        auto mio_read_res = mio_backend->read(mio_data);
        auto mem_read_res = mem_backend->read(mem_data);

        ASSERT_TRUE(mio_read_res.has_value());
        ASSERT_TRUE(mem_read_res.has_value());
        ASSERT_EQ(*mio_read_res, final_size);
        ASSERT_EQ(*mem_read_res, final_size);

        ASSERT_EQ(mio_data, mem_data);
    }

    // Cleanup
    mio_backend.reset();
    if (fs::exists(test_filepath)) {
        fs::remove(test_filepath);
    }
}