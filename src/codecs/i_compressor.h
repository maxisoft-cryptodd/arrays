#pragma once

#include "../memory/allocator.h" // Include to get the definition of memory::vector
#include <expected>
#include <span>
#include <string>
#include <vector>
#include <format>

namespace cryptodd {

class ICompressor {
public:
    virtual ~ICompressor() = default;

    /**
     * @brief Compresses data directly into a vector with a specific allocator, avoiding copies.
     */
    template <typename TAllocator>
    std::expected<std::vector<std::byte, TAllocator>, std::string> compress_to(std::span<const std::byte> uncompressed_data) {
        const size_t bound = this->do_get_compress_bound(uncompressed_data);
        std::vector<std::byte, TAllocator> compressed_data(bound);

        auto result = this->do_compress_into(uncompressed_data, compressed_data);
        if (!result) {
            return std::unexpected(result.error());
        }

        compressed_data.resize(*result);
        return compressed_data;
    }

    /**
     * @brief Decompresses data directly into a vector with a specific allocator, avoiding copies.
     */
    template <typename TAllocator>
    std::expected<std::vector<std::byte, TAllocator>, std::string> decompress_to(std::span<const std::byte> compressed_data) {
        auto expected_size = this->do_get_decompress_size(compressed_data);
        if (!expected_size) {
            return std::unexpected(expected_size.error());
        }

        std::vector<std::byte, TAllocator> decompressed_data(*expected_size);
        auto result = this->do_decompress_into(compressed_data, decompressed_data);
        if (!result) {
            return std::unexpected(result.error());
        }

        if (*result != *expected_size) {
             return std::unexpected(
                std::format("Decompression size mismatch. Expected {}, got {}.", *expected_size, *result));
        }

        return decompressed_data;
    }

    // --- Backward Compatibility API (using memory::vector) ---
    // These now have default implementations that call the templated versions with the correct allocator type.
    virtual std::expected<memory::vector<std::byte>, std::string> compress(std::span<const std::byte> uncompressed_data) {
        // Deduces the allocator (either std::allocator or mi_stl_allocator) from the alias.
        using Allocator = memory::vector<std::byte>::allocator_type;
        return this->compress_to<Allocator>(uncompressed_data);
    }

    virtual std::expected<memory::vector<std::byte>, std::string> decompress(std::span<const std::byte> compressed_data) {
        // Deduces the allocator (either std::allocator or mi_stl_allocator) from the alias.
        using Allocator = memory::vector<std::byte>::allocator_type;
        return this->decompress_to<Allocator>(compressed_data);
    }

protected:
    // --- New Core Virtual Interface for derived classes ---
    // These methods operate on raw spans, allowing the caller to manage allocation.
    virtual size_t do_get_compress_bound(std::span<const std::byte> uncompressed_data) = 0;
    virtual std::expected<size_t, std::string> do_get_decompress_size(std::span<const std::byte> compressed_data) = 0;
    
    virtual std::expected<size_t, std::string> do_compress_into(std::span<const std::byte> uncompressed, std::span<std::byte> compressed) = 0;
    virtual std::expected<size_t, std::string> do_decompress_into(std::span<const std::byte> compressed, std::span<std::byte> decompressed) = 0;
};

}
