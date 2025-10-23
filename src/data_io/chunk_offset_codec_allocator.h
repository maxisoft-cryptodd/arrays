#pragma once

#include "../memory/object_allocator.h"
#include "../codecs/codec_cache.h"
#include "chunk_offset_codec_allocator_fwd.h"

namespace cryptodd {

    // Define the ObjectAllocator type for ChunkOffsetCodecCache
    using ChunkOffsetCodecAllocator = memory::ObjectAllocator<ChunkOffsetCodecCache>;

} // namespace cryptodd
