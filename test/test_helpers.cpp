#include "test_helpers.h"

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