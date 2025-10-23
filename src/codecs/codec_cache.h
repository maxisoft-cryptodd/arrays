#pragma once

#include "temporal_1d_simd_codec.h"
#include "zstd_compressor.h"

namespace cryptodd
{

    template <int DefaultCompressionLevel>
    struct CodecCache1d
    {
        Temporal1dSimdCodecWorkspace workspace;
        Temporal1dSimdCodec codec;

        // Default constructor to satisfy the PoolableObject concept.
        // This is what ObjectAllocator uses to create new instances.
        CodecCache1d() : CodecCache1d(DefaultCompressionLevel)
        {
        }

        // Constructor for DataWriter's specific ZstdCompressor initialization
        explicit CodecCache1d(int compression_level) :
            codec(std::make_unique<ZstdCompressor>(compression_level))
        {
        }
    };

    using DefaultCodecCache1d = CodecCache1d<ZstdCompressor::DEFAULT_COMPRESSION_LEVEL>;

} // namespace cryptodd
