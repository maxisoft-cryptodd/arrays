#include "chunk_offset_codec_allocator.h"
#include <mutex> // For std::call_once

namespace cryptodd {

    namespace {
        // Static ObjectAllocator instance for ChunkOffsetCodecCache
        ChunkOffsetCodecAllocator static_chunk_offset_codec_allocator{};
    }

    // Implementation of the default allocator getter
    std::shared_ptr<ChunkOffsetCodecAllocator> get_chunk_offset_codec_allocator() {
        // Return a shared_ptr with a no-op deleter to the static allocator
        return {&static_chunk_offset_codec_allocator, [](auto*) {}};
    }

} // namespace cryptodd
