#include "data_reader.h"

#include "blake3_stream_hasher.h"
#include "../storage/file_backend.h"
#include "../storage/memory_backend.h"
#include "../file_format/serialization_helpers.h"

#include <filesystem> // For std::filesystem::exists
#include <format>
#include <span>

#include "../codecs/temporal_1d_simd_codec.h"
#include "../memory/object_allocator.h"
#include "../codecs/codec_cache.h"

namespace cryptodd {

namespace {
    // Static ObjectAllocator instance for CodecCache
    memory::ObjectAllocator<DefaultCodecCache1d> static_codec_cache_allocator{};
}

// Implementation of the default allocator getter
std::shared_ptr<memory::ObjectAllocator<DefaultCodecCache1d>> get_default_codec_allocator()
{
    // Return a shared_ptr with a no-op deleter to the static allocator
    return {&static_codec_cache_allocator, [](auto*) {}};
}

ZstdCompressor& DataReader::get_zstd_compressor() const
{
    std::call_once(zstd_init_flag_, [this]() { zstd_compressor_.emplace(); });
    return *zstd_compressor_;
}

DataReader::DataReader(Create, std::unique_ptr<IStorageBackend>&& backend,
                       std::shared_ptr<memory::ObjectAllocator<DefaultCodecCache1d>> codec_allocator) :
    backend_(std::move(backend)), codec_cache_allocator_(std::move(codec_allocator))
{
    auto read_header = [this]() -> std::expected<void, std::string>
    {
        if (auto res = file_header_.read(*backend_); !res)
        {
            return std::unexpected(res.error());
        }

        auto tell_res = backend_->tell();
        if (!tell_res)
        {
            return std::unexpected(tell_res.error());
        }

        index_block_offset_ = *tell_res;
        uint64_t total_index_size = 0;
        uint64_t current_block_offset = *tell_res;

        while (current_block_offset != 0)
        {
            if (auto seek_res = backend_->seek(current_block_offset); !seek_res)
            {
                return std::unexpected(seek_res.error());
            }

            auto size_res = serialization::read_pod<uint32_t>(*backend_);
            if (!size_res)
            {
                return std::unexpected("Failed to read block size: " + size_res.error());
            }
            const uint32_t block_size_on_disk = *size_res;

            auto type_res = serialization::read_pod<ChunkOffsetType>(*backend_);
            if (!type_res)
            {
                return std::unexpected("Failed to read block type: " + type_res.error());
            }
            const auto block_type = *type_res;

            auto hash_res = serialization::read_pod<blake3_hash256_t>(*backend_);
            if (!hash_res)
            {
                return std::unexpected("Failed to read block hash: " + hash_res.error());
            }
            const auto block_hash = *hash_res;

            auto next_offset_res = serialization::read_pod<uint64_t>(*backend_);
            if (!next_offset_res)
            {
                return std::unexpected("Failed to read next block offset: " + next_offset_res.error());
            }

            total_index_size += block_size_on_disk;
            memory::vector<uint64_t> offsets;

            if (block_type == ChunkOffsetType::RAW)
            {
                auto payload_res = serialization::read_vector_pod<uint64_t>(*backend_);
                if (!payload_res)
                {
                    return std::unexpected("Failed to read RAW block payload: " + payload_res.error());
                }
                offsets = std::move(*payload_res);

                const auto raw_payload_bytes =
                    serialization::serialize_vector_pod_to_buffer(std::span<const uint64_t>(offsets));
                Blake3StreamHasher block_hasher;
                block_hasher.update(std::span<const std::byte>(raw_payload_bytes));
                if (block_hasher.finalize_256() != block_hash)
                {
                    return std::unexpected("RAW ChunkOffsetsBlock integrity check failed.");
                }
            }
            else if (block_type == ChunkOffsetType::ZSTD_COMPRESSED)
            {
                auto compressed_blob_res = serialization::read_blob(*backend_);
                if (!compressed_blob_res)
                {
                    return std::unexpected("Failed to read ZSTD blob: " + compressed_blob_res.error());
                }

                const size_t header_and_ptr_size =
                    sizeof(uint32_t) + sizeof(uint16_t) + sizeof(blake3_hash256_t) + sizeof(uint64_t);
                const size_t raw_payload_size = block_size_on_disk - header_and_ptr_size;
                const size_t num_elements = (raw_payload_size - sizeof(uint32_t)) / sizeof(uint64_t);

                auto cache_ptr = codec_cache_allocator_->acquire();
                int64_t prev_element = 0;
                auto decoded_res = cache_ptr->codec.decode64_Delta(*compressed_blob_res, num_elements, prev_element);
                if (!decoded_res)
                {
                    return std::unexpected("SIMD delta decoding failed: " + decoded_res.error());
                }

                const auto raw_payload_bytes =
                    serialization::serialize_vector_pod_to_buffer(std::span<const int64_t>(*decoded_res));
                Blake3StreamHasher block_hasher;
                block_hasher.update(std::span<const std::byte>(raw_payload_bytes));
                if (block_hasher.finalize_256() != block_hash)
                {
                    return std::unexpected("ZSTD ChunkOffsetsBlock integrity check failed.");
                }

                offsets.assign(decoded_res->begin(), decoded_res->end());
            }
            else
            {
                return std::unexpected("Unknown ChunkOffsetsBlock type.");
            }

            for (const auto offset : offsets)
            {
                if (offset != 0)
                {
                    master_chunk_offsets_.push_back(offset);
                }
                else
                {
                    break;
                }
            }
            current_block_offset = *next_offset_res;
        }

        index_block_size_ = total_index_size;
        return {};
    }();

    if (!read_header)
    {
        throw std::runtime_error(read_header.error());
    }
}

std::expected<std::unique_ptr<DataReader>, std::string> DataReader::open(const std::filesystem::path& filepath)
{
    if (!std::filesystem::exists(filepath))
    {
        return std::unexpected("File does not exist: " + filepath.string());
    }
    try
    {
        auto backend = std::make_unique<storage::FileBackend>(filepath, std::ios_base::in | std::ios_base::binary);
        return std::make_unique<DataReader>(Create{}, std::move(backend));
    }
    catch (const std::exception& e)
    {
        return std::unexpected(std::format("Failed to open file '{}': {}", filepath.string(), e.what()));
    }
}
std::expected<std::unique_ptr<DataReader>, std::string> DataReader::open_in_memory(
    std::unique_ptr<IStorageBackend> backend)
{
    if (!backend)
    {
        return std::unexpected("Provided backend is null.");
    }
    try
    {
        return std::make_unique<DataReader>(Create{}, std::move(backend));
    }
    catch (const std::exception& e)
    {
        return std::unexpected(std::format("Failed to open in-memory reader: {}", e.what()));
    }
}

std::expected<std::unique_ptr<DataReader>, std::string> DataReader::open_in_memory()
{
    return open_in_memory(std::move(std::make_unique<storage::MemoryBackend>()));
}

std::expected<Chunk, std::string> DataReader::get_chunk(const size_t index)
{
    if (index >= master_chunk_offsets_.size())
    {
        return std::unexpected(
            std::format("Chunk index {} is out of range (total chunks: {}).", index, master_chunk_offsets_.size()));
    }

    uint64_t chunk_offset = master_chunk_offsets_[index];
    if (auto seek_res = backend_->seek(chunk_offset); !seek_res)
    {
        return std::unexpected(seek_res.error());
    }

    Chunk chunk;
    if (auto read_res = chunk.read(*backend_); !read_res)
    {
        return std::unexpected(read_res.error());
    }

    if (chunk.shape().size() > MAX_SHAPE_DIMENSIONS)
    {
        return std::unexpected(
            std::format("Chunk {} shape has an excessive number of dimensions (> {}). File may be corrupt.", index,
                        MAX_SHAPE_DIMENSIONS));
    }

    return chunk;
}

std::expected<memory::vector<memory::vector<std::byte>>, std::string> DataReader::get_chunk_slice(size_t start_index,
                                                                                                  size_t end_index)
{
    if (start_index >= master_chunk_offsets_.size() || start_index > end_index)
    {
        return std::unexpected("Invalid slice indices for get_chunk_slice.");
    }
    if (end_index >= master_chunk_offsets_.size())
    {
        end_index = master_chunk_offsets_.size() - 1;
    }

    memory::vector<memory::vector<std::byte>> slice_data;
    for (size_t i = start_index; i <= end_index; ++i)
    {
        auto chunk_result = get_chunk(i);
        if (!chunk_result)
        {
            return std::unexpected(std::format("Failed to retrieve chunk {} for slice: {}", i, chunk_result.error()));
        }
        slice_data.push_back(std::move(chunk_result->data()));
    }
    return slice_data;
}

} // namespace cryptodd