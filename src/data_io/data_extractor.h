#pragma once

#include <expected>
#include <memory>

#include "buffer.h"

namespace cryptodd
{

class DataExtractor
{
    // Using the PIMPL idiom to hide implementation details and private members.
    struct Impl;
    std::unique_ptr<Impl> pimpl_;

  public:
    DataExtractor();
    ~DataExtractor();
    DataExtractor(DataExtractor&&) noexcept;
    DataExtractor& operator=(DataExtractor&&) noexcept;

    using BufferResult = std::expected<std::unique_ptr<Buffer>, std::string>;
    /**
     * @brief Reads and decodes a chunk of data.
     * @param chunk The chunk to process. The chunk's data will be moved out.
     * @return A unique_ptr to a Buffer containing the decoded data, or an error string.
     */
    BufferResult read_chunk(Chunk& chunk);
};

} // namespace cryptodd
