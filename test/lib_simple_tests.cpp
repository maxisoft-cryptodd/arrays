#include <gtest/gtest.h>
#include <zstd.h> // For zstd-cpp
#include <lz4.h>  // For lz4
#include <blake3.h> // For blake3
#include <vector>
#include <string>
#include <cstring> // For strlen, memcmp
#include <cstdint> // For uint8_t

// Test for zstd-cpp
TEST(ZstdCppTest, CompressionDecompression) {
    const char* original_data = "This is a test string for zstd compression.";
    size_t original_size = strlen(original_data);

    // Estimate compressed size
    size_t compressed_size_bound = ZSTD_compressBound(original_size);
    std::vector<char> compressed_data(compressed_size_bound);

    // Compress
    size_t compressed_size = ZSTD_compress(compressed_data.data(), compressed_size_bound, original_data, original_size, 1);
    ASSERT_FALSE(ZSTD_isError(compressed_size)) << "ZSTD Compression failed: " << ZSTD_getErrorName(compressed_size);

    // Decompress
    std::vector<char> decompressed_data(original_size);
    size_t decompressed_size = ZSTD_decompress(decompressed_data.data(), original_size, compressed_data.data(), compressed_size);
    ASSERT_FALSE(ZSTD_isError(decompressed_size)) << "ZSTD Decompression failed: " << ZSTD_getErrorName(decompressed_size);

    // Verify
    ASSERT_EQ(original_size, decompressed_size);
    // Use memcmp for safe comparison of raw buffers, not ASSERT_STREQ
    ASSERT_EQ(0, memcmp(original_data, decompressed_data.data(), original_size));
}

// Test for lz4
TEST(LZ4Test, CompressionDecompression) {
    const char* original_data = "This is a test string for lz4 compression.";
    size_t original_size = strlen(original_data);

    // LZ4_COMPRESSBOUND and LZ4_compress_default expect int for size.
    // Cast original_size to int. For typical test strings, this is safe.
    int original_size_int = static_cast<int>(original_size);

    // Allocate buffer for compressed data. Max size is original_size + overhead.
    std::vector<char> compressed_data(LZ4_COMPRESSBOUND(original_size_int));

    // Compress
    // LZ4_compress_default expects int for size.
    int compressed_size = LZ4_compress_default(original_data, compressed_data.data(), original_size_int, static_cast<int>(compressed_data.size()));
    ASSERT_GT(compressed_size, 0) << "LZ4 Compression failed";

    // Decompress
    std::vector<char> decompressed_data(original_size);
    // LZ4_decompress_safe expects int for compressed size and original size.
    int decompressed_size = LZ4_decompress_safe(compressed_data.data(), decompressed_data.data(), compressed_size, original_size_int);
    ASSERT_EQ(original_size_int, decompressed_size) << "LZ4 Decompression failed"; // Decompression should return the original size

    // Verify
    // Use memcmp for safe comparison of raw buffers, not ASSERT_STREQ
    ASSERT_EQ(0, memcmp(original_data, decompressed_data.data(), original_size));
}

// Test for blake3
TEST(Blake3Test, Hashing) {
    const char* data_to_hash = "hello";
    size_t data_size = strlen(data_to_hash);

    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data_to_hash, data_size);

    uint8_t output[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&hasher, output, BLAKE3_OUT_LEN);

    // Convert the hash bytes to a hex string for comparison.
    std::string calculated_hash_hex;
    calculated_hash_hex.reserve(BLAKE3_OUT_LEN * 2);
    for (size_t i = 0; i < BLAKE3_OUT_LEN; ++i) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", output[i]);
        calculated_hash_hex += buf;
    }

    // The known BLAKE3 hash for "hello"
    const std::string expected_hash_hex = "ea8f163db38682925e4491c5e58d4bb3506ef8c14eb78a86e908c5624a67200f";
    ASSERT_EQ(calculated_hash_hex, expected_hash_hex);
}
