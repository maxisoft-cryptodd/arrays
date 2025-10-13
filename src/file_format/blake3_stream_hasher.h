#pragma once

#include <array>
#include <span>
#include <stdexcept>
#include "../memory/allocator.h"
#include <memory>

namespace cryptodd {

using blake3_hash128_t = std::array<uint64_t, 2>;

/**
 * @brief A stateful C++ wrapper for the blake3 streaming hash API.
 */
class Blake3StreamHasher {
public:
    Blake3StreamHasher();
    ~Blake3StreamHasher();
    Blake3StreamHasher(Blake3StreamHasher&&) noexcept;
    Blake3StreamHasher& operator=(Blake3StreamHasher&&) noexcept;

    // Resets the hasher to its initial state, allowing it to be reused.
    void reset();

    // Updates the hash state with new data.
    template<typename T>
    void update(std::span<const T> data) {
        update_bytes({reinterpret_cast<const std::byte*>(data.data()), data.size_bytes()});
    }

    /**
     * @brief Finalizes the hash and returns a byte array of a specified length.
     * @tparam N The desired output length in bytes.
     * @return A std::array<std::byte, N> containing the hash.
     */
    template<size_t N>
    [[nodiscard]] std::array<std::byte, N> finalize() const {
        std::array<std::byte, N> hash_bytes{};
        finalize_to_span(hash_bytes);
        return hash_bytes;
    }

    /**
     * @brief Finalizes the hash and returns a byte vector of a specified length.
     * @param out_len The desired output length in bytes.
     * @return A cryptodd::memory::vector<std::byte> containing the hash.
     */
    [[nodiscard]] memory::vector<std::byte> finalize(size_t out_len) const;

    /**
     * @brief Finalizes the hash and returns a 128-bit (16-byte) hash.
     * @return A blake3_hash128_t containing the hash.
     */
    [[nodiscard]] blake3_hash128_t finalize_128() const;

    void update_bytes(std::span<const std::byte> data);

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;

    // Initializes or re-initializes the hasher state.
    void init();

    void finalize_to_span(std::span<std::byte> out) const;
};

template<typename T>
blake3_hash128_t calculate_blake3_hash128(std::span<const T> d) {
    Blake3StreamHasher h;
    h.update(d);
    return h.finalize_128();
}

} // namespace cryptodd
