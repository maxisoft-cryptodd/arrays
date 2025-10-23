#pragma once
#include "../memory/object_allocator.h"

namespace cryptodd {
    // Define the ZSTD compression level for chunk offset blocks
    constexpr int CHUNK_OFFSETS_BLOCK_ZSTD_COMPRESSION_LEVEL = -2; // Or whatever the desired level is

    template <int DefaultCompressionLevel>
        struct CodecCache1d;

    using ChunkOffsetCodecCache = CodecCache1d<CHUNK_OFFSETS_BLOCK_ZSTD_COMPRESSION_LEVEL>;
    using ChunkOffsetCodecAllocator = memory::ObjectAllocator<ChunkOffsetCodecCache>;

    std::shared_ptr<ChunkOffsetCodecAllocator> get_chunk_offset_codec_allocator();

} // namespace cryptodd
