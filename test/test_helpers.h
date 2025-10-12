#pragma once

#include <vector>
#include <filesystem>
#include <cstddef>

// Helper to generate random data
std::vector<std::byte> generate_random_data(size_t size);

// Helper to create a unique temporary filepath for tests.
std::filesystem::path generate_unique_test_filepath();