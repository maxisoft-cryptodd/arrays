#include "test_helpers.h"
#include "../src/data_io/data_reader.h"
#include "../src/codecs/zstd_compressor.h"

#include <random>
#include <array>
#include <string>
#include <algorithm>

#include <stduuid/uuid.h>

cryptodd::memory::vector<std::byte> generate_random_data(size_t size) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> distrib(0, 255);

    cryptodd::memory::vector<std::byte> data(size);
    std::ranges::generate(data, [&]() {
        return static_cast<std::byte>(distrib(gen));
    });
    return data;
}

std::filesystem::path generate_unique_test_filepath() {
    std::random_device rd;
    auto seed_data = std::array<int, std::mt19937::state_size>{};
    std::ranges::generate(seed_data, std::ref(rd));
    std::seed_seq seq(std::begin(seed_data), std::end(seed_data));
    std::mt19937 generator(seq);
    uuids::uuid_random_generator gen{generator};

    const uuids::uuid id = gen();
    const std::string filename = "cryptodd_test_" + uuids::to_string(id) + ".cdd";
    return std::filesystem::temp_directory_path() / filename;
}

::testing::AssertionResult UserMetadataMatches(const cryptodd::DataReader& reader, std::span<const std::byte> expected_meta) {
    const auto& compressed_meta = reader.get_file_header().user_metadata();
    if (compressed_meta.empty() && expected_meta.empty()) {
        return ::testing::AssertionSuccess();
    }
    cryptodd::ZstdCompressor compressor;
    auto decompressed_result = compressor.decompress(compressed_meta);
    if (!decompressed_result) {
        return ::testing::AssertionFailure() << "Failed to decompress user metadata: " << decompressed_result.error();
    }
    if (std::equal(decompressed_result->begin(), decompressed_result->end(), expected_meta.begin(), expected_meta.end())) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << "User metadata does not match.";
}

cryptodd::blake3_hash256_t calculate_blake3_hash256(std::span<const std::byte> data) {
    cryptodd::Blake3StreamHasher hasher;
    hasher.update(data);
    return hasher.finalize_256();
}