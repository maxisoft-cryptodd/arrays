#pragma once

#include <expected>
#include <memory>

#include "buffer.h"
#include "codec_error.h" // Include the new error header
#include <expected>
#include <memory>
#include <span>

namespace cryptodd
{

class DataExtractor
{
    struct Impl;
    std::unique_ptr<Impl> pimpl_;

  public:
    DataExtractor();
    ~DataExtractor();
    DataExtractor(DataExtractor&&) noexcept;
    DataExtractor& operator=(DataExtractor&&) noexcept;

    using BufferResult = std::expected<std::unique_ptr<Buffer>, CodecError>;
    
    // Stateless overload (default zero-init for prev_state)
    BufferResult read_chunk(Chunk& chunk);

    // Overloads for stateful decoding of temporal data
    BufferResult read_chunk(Chunk& chunk, float& prev_element); // For 1D float
    BufferResult read_chunk(Chunk& chunk, int64_t& prev_element); // For 1D int64
    BufferResult read_chunk(Chunk& chunk, std::span<float> prev_row); // For 2D float or Orderbook
    BufferResult read_chunk(Chunk& chunk, std::span<int64_t> prev_row); // For 2D int64
};

}
