#pragma once

#include <array>
#include <blake3.h>
#include <span>
#include <stdexcept>
#include <vector>

namespace cryptodd {

using blake3_hash128_t = std::array<uint64_t, 2>;

/**
 * @brief A stateful C++ wrapper for the blake3 streaming hash API.
 */
class Blake3StreamHasher {
private:
    blake3_hasher hasher_{};
    bool is_initialized_ = false;

public:
    Blake3StreamHasher() = default;

    // Initializes or re-initializes the hasher state.
    void init() {
        blake3_hasher_init(&hasher_);
        is_initialized_ = true;
    }

    // Updates the hash state with new data.
    template<typename T>
    void update(std::span<const T> data) {
        if (!is_initialized_) {
            init();
        }
        blake3_hasher_update(&hasher_, data.data(), data.size_bytes());
    }

    /**
     * @brief Finalizes the hash and returns a byte array of a specified length.
     * @tparam N The desired output length in bytes.
     * @return A std::array<uint8_t, N> containing the hash.
     */
    template<size_t N>
    [[nodiscard]] std::array<uint8_t, N> finalize() const {
        if (!is_initialized_) {
            throw std::logic_error("Hasher has not been initialized before finalizing.");
        }
        std::array<uint8_t, N> hash_bytes{};
        blake3_hasher_finalize(&hasher_, hash_bytes.data(), N);
        return hash_bytes;
    }

    /**
     * @brief Finalizes the hash and returns a 128-bit (16-byte) hash.
     * @return A blake3_hash128_t containing the hash.
     */
    [[nodiscard]] blake3_hash128_t finalize_128() const
    {
        if (!is_initialized_) {
            throw std::logic_error("Hasher has not been initialized before finalizing.");
        }
        blake3_hash128_t hash_u64{};
        blake3_hasher_finalize(&hasher_, reinterpret_cast<uint8_t*>(&hash_u64), sizeof(hash_u64));
        return hash_u64;
    }
};

template<typename T>
blake3_hash128_t calculate_blake3_hash128(std::span<const T> d) {
    Blake3StreamHasher h;
    h.update(d);
    return h.finalize_128();
}

} // namespace cryptodd