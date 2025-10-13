#pragma once

#include "allocator.h"

#include <cstddef>
#include <filesystem>
#include <vector>

// Helper to generate random data
cryptodd::memory::vector<std::byte> generate_random_data(size_t size);

// Helper to create a unique temporary filepath for tests.
std::filesystem::path generate_unique_test_filepath();