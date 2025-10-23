#pragma once

#include "../src/memory/allocator.h"
#include <gtest/gtest.h>
#include <cstddef>
#include <filesystem>
#include <vector>
#include <span>

#include "../src/file_format/blake3_stream_hasher.h"

namespace cryptodd { class DataReader; }

// Helper to generate random data
cryptodd::memory::vector<std::byte> generate_random_data(size_t size);

// Helper to create a unique temporary filepath for tests.
std::filesystem::path generate_unique_test_filepath();

// Helper to calculate a blake3 hash
cryptodd::blake3_hash256_t calculate_blake3_hash256(std::span<const std::byte> data);

// Custom predicate assertion for verifying user metadata.
::testing::AssertionResult UserMetadataMatches(const cryptodd::DataReader& reader, std::span<const std::byte> expected_meta);