#pragma once

#include <span>
#include <vector>

namespace cryptodd {

class ICompressor {
public:
    virtual ~ICompressor() = default;

    /**
     * @brief Compresses a block of data.
     * @param uncompressed_data The raw bytes to compress.
     * @return A vector containing the compressed bytes.
     */
    virtual std::vector<uint8_t> compress(std::span<const uint8_t> uncompressed_data) = 0;

    /**
     * @brief Decompresses a block of data.
     * @param compressed_data The compressed bytes to decompress.
     * @return A vector containing the original, decompressed bytes.
     */
    virtual std::vector<uint8_t> decompress(std::span<const uint8_t> compressed_data) = 0;
};

} // namespace cryptodd