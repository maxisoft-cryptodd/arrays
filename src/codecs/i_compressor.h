#pragma once

#include <span>
#include <cstddef>
#include <vector>
#include <expected>
#include <string>
#include <cstddef>

namespace cryptodd {

class ICompressor {
public:
    virtual ~ICompressor() = default;

    /**
     * @brief Compresses a block of data.
     * @param uncompressed_data The raw bytes to compress.
     * @return A std::expected containing either the compressed bytes or an error string.
     */
    virtual std::expected<std::vector<std::byte>, std::string> compress(std::span<const std::byte> uncompressed_data) = 0;

    /**
     * @brief Decompresses a block of data.
     * @param compressed_data The compressed bytes to decompress.
     * @return A std::expected containing either the original, decompressed bytes or an error string.
     */
    virtual std::expected<std::vector<std::byte>, std::string> decompress(std::span<const std::byte> compressed_data) = 0;
};

} // namespace cryptodd
