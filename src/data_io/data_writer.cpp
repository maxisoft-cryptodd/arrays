#include "data_writer.h"

#include "../file_format/serialization_helpers.h"
#include "../storage/file_backend.h"
#include "../storage/i_storage_backend.h"
#include "../storage/memory_backend.h"
#include "../file_format/blake3_stream_hasher.h"
#include "../codecs/zstd_compressor.h"

#include <format>
#include <memory> // For std::make_unique

#include "../codecs/temporal_1d_simd_codec.h"

namespace cryptodd {

namespace {
    constexpr int CHUNK_OFFSETS_BLOCK_ZSTD_COMPRESSION = -2;

    struct CodecCache {
        Temporal1dSimdCodecWorkspace workspace;
        Temporal1dSimdCodec codec{std::make_unique<ZstdCompressor>(CHUNK_OFFSETS_BLOCK_ZSTD_COMPRESSION)};
    };

    auto& get_codec_cache() {
        thread_local CodecCache cache;
        return cache;
    }
}

// --- Private Methods ---

ZstdCompressor& DataWriter::get_zstd_compressor() const {
    std::call_once(zstd_init_flag_, [this]() { zstd_compressor_.emplace(CHUNK_OFFSETS_BLOCK_ZSTD_COMPRESSION); });
    return *zstd_compressor_;
}

std::expected<void, std::string> DataWriter::write_new_chunk_offsets_block(uint64_t previous_block_offset) {
    const auto original_pos = backend_->tell();
    if (!original_pos) return std::unexpected(original_pos.error());

    // --- Finalize and Write Previous Block (if it exists) ---
    if (previous_block_offset != 0 && !chunk_offset_blocks_.empty()) {
        auto& prev_block = chunk_offset_blocks_.back();
        prev_block.set_next_index_offset(*original_pos);

        const auto raw_offsets_payload = serialization::serialize_vector_pod_to_buffer(std::span(prev_block.offsets()));
        
        Blake3StreamHasher hasher;
        hasher.update(std::span<const std::byte>(raw_offsets_payload));
        prev_block.set_hash(hasher.finalize_256());

        auto& cache = get_codec_cache();
        auto& offsets = prev_block.offsets(); // memory::vector<uint64_t>

        bool can_use_delta_encoding = true;
        if (!offsets.empty() && offsets.back() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            can_use_delta_encoding = false; // Overflow but very unlikely to happen
        }

        if (can_use_delta_encoding) {
            std::span<const int64_t> offsets_as_i64(reinterpret_cast<const int64_t*>(offsets.data()), offsets.size());
            
            auto compressed_res = cache.codec.encode64_Delta(offsets_as_i64, 0, cache.workspace);
            if (!compressed_res) {
                return std::unexpected("SIMD delta encoding failed: " + compressed_res.error());
            }
            auto& compressed_payload = *compressed_res;

            const size_t compressed_payload_disk_size = sizeof(uint32_t) + compressed_payload.size();
            if (compressed_payload_disk_size < raw_offsets_payload.size()) {
                prev_block.set_type(ChunkOffsetType::ZSTD_COMPRESSED);
                
                if (auto res = backend_->seek(previous_block_offset); !res) return res;

                if (auto res = serialization::write_pod(*backend_, prev_block.size()); !res) return std::unexpected(res.error());
                if (auto res = serialization::write_pod(*backend_, static_cast<uint16_t>(prev_block.type())); !res) return std::unexpected(res.error());
                if (auto res = serialization::write_pod(*backend_, prev_block.hash()); !res) return std::unexpected(res.error());
                if (auto res = serialization::write_pod(*backend_, prev_block.get_next_index_offset()); !res) return std::unexpected(res.error());
                if (auto res = serialization::write_blob(*backend_, compressed_payload); !res) return std::unexpected(res.error());

                const size_t header_plus_next_ptr_size = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(blake3_hash256_t) + sizeof(uint64_t);
                const uint64_t end_of_compressed_data = previous_block_offset + header_plus_next_ptr_size + compressed_payload_disk_size;
                const uint64_t end_of_block = previous_block_offset + prev_block.size();

                if (end_of_compressed_data < end_of_block) {
                    const size_t padding_size = end_of_block - end_of_compressed_data;
                    const memory::vector<std::byte> zeros(padding_size, std::byte{0});
                    if (auto res = backend_->write(zeros); !res) return std::unexpected("Failed to write zero padding: " + res.error());
                }
            } else {
                // Fallback to RAW because compression was not beneficial
                prev_block.set_type(ChunkOffsetType::RAW);
                if (auto res = backend_->seek(previous_block_offset); !res) return res;
                if (auto res = prev_block.write(*backend_); !res) return std::unexpected("Failed to write RAW block update: " + res.error());
            }
        } else {
            // Fallback to RAW because offsets exceed int64_t max, cannot use delta encoding
            prev_block.set_type(ChunkOffsetType::RAW);
            if (auto res = backend_->seek(previous_block_offset); !res) return res;
            if (auto res = prev_block.write(*backend_); !res) return std::unexpected("Failed to write RAW block update: " + res.error());
        }
    }

    if (auto res = backend_->seek(*original_pos); !res) return res;

    ChunkOffsetsBlock new_block;
    new_block.set_type(ChunkOffsetType::RAW);
    new_block.set_offsets(memory::vector<uint64_t>(chunk_offsets_block_capacity_, 0));
    new_block.set_next_index_offset(0);

    const size_t raw_payload_size = new_block.get_raw_payload_size();
    const size_t total_block_size = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(blake3_hash256_t) + sizeof(uint64_t) + raw_payload_size;
    new_block.set_size(static_cast<uint32_t>(total_block_size));

    const auto raw_payload_bytes = serialization::serialize_vector_pod_to_buffer(std::span(new_block.offsets()));
    Blake3StreamHasher initial_hasher;
    // ReSharper disable once CppTemplateArgumentsCanBeDeduced
    initial_hasher.update(std::span<const std::byte>(raw_payload_bytes));
    new_block.set_hash(initial_hasher.finalize_256());

    if (auto res = new_block.write(*backend_); !res) {
        return std::unexpected("Failed to write new chunk offsets block: " + res.error());
    }

    current_chunk_offset_block_start_ = *original_pos;
    chunk_offset_blocks_.push_back(std::move(new_block));
    current_chunk_offset_block_index_ = 0;

    return {};
}

// --- Public Methods ---

void DataWriter::set_compression_level(int level) {
    get_zstd_compressor().set_level(level);
}

std::expected<std::unique_ptr<DataWriter>, std::string> DataWriter::create_new(const std::filesystem::path& filepath,
                                                                              size_t chunk_offsets_block_capacity,
                                                                              std::span<const std::byte> user_metadata) {
    if (std::filesystem::exists(filepath)) {
        return std::unexpected("File already exists: " + filepath.string() + ". Use open_for_append for existing files.");
    }
    try {
        auto backend = std::make_unique<storage::FileBackend>(filepath, std::ios_base::out | std::ios_base::binary |
                                                                            std::ios_base::trunc);
        return std::make_unique<DataWriter>(Create{}, std::move(backend), chunk_offsets_block_capacity, user_metadata);
    } catch (const std::exception& e) {
        return std::unexpected(std::format("Failed to create new file '{}': {}", filepath.string(), e.what()));
    }
}

std::expected<std::unique_ptr<DataWriter>, std::string> DataWriter::open_for_append(const std::filesystem::path& filepath) {
    if (!std::filesystem::exists(filepath)) {
        return std::unexpected("File does not exist: " + filepath.string() + ". Use create_new for new files.");
    }
    try {
        auto backend = std::make_unique<storage::FileBackend>(filepath, std::ios_base::in | std::ios_base::out |
                                                                            std::ios_base::binary);
        return std::make_unique<DataWriter>(Create{}, std::move(backend));
    } catch (const std::exception& e) {
        return std::unexpected(std::format("Failed to open file for append '{}': {}", filepath.string(), e.what()));
    }
}

std::expected<std::unique_ptr<DataWriter>, std::string> DataWriter::create_in_memory(size_t chunk_offsets_block_capacity,
                                                                                    std::span<const std::byte> user_metadata) {
    try {
        auto backend = std::make_unique<storage::MemoryBackend>();
        return std::make_unique<DataWriter>(Create{}, std::move(backend), chunk_offsets_block_capacity, user_metadata);
    } catch (const std::exception& e) {
        return std::unexpected(std::format("Failed to create in-memory writer: {}", e.what()));
    }
}

DataWriter::DataWriter(Create, std::unique_ptr<IStorageBackend>&& backend, size_t chunk_offsets_block_capacity,
                       std::span<const std::byte> user_metadata)
    : backend_(std::move(backend)), chunk_offsets_block_capacity_(chunk_offsets_block_capacity) {
    auto init = [this, user_metadata]() -> std::expected<void, std::string> {
        InternalMetadata internal_meta{};
        internal_meta.chunk_offsets_block_capacity = chunk_offsets_block_capacity_;

        storage::MemoryBackend temp_meta_backend;
        if (auto res = serialization::write_pod(temp_meta_backend, internal_meta.chunk_offsets_block_capacity); !res) return std::unexpected(res.error());
        if (auto res = temp_meta_backend.rewind(); !res) return res;
        auto size_res = temp_meta_backend.size();
        if (!size_res) return std::unexpected(size_res.error());
        std::vector<std::byte> serialized_internal_meta(*size_res);
        if (auto res = temp_meta_backend.read(serialized_internal_meta); !res) return std::unexpected(res.error());

        auto& compressor = get_zstd_compressor();
        auto user_meta_res = compressor.compress(user_metadata);
        if (!user_meta_res) return std::unexpected("Failed to compress user metadata: " + user_meta_res.error());
        file_header_.set_user_metadata(std::move(*user_meta_res));

        auto internal_meta_res = compressor.compress(serialized_internal_meta);
        if (!internal_meta_res)
            return std::unexpected("Failed to compress internal metadata: " + internal_meta_res.error());
        file_header_.set_internal_metadata(std::move(*internal_meta_res));

        if (auto res = file_header_.write(*backend_); !res) return res;

        return write_new_chunk_offsets_block(0);
    }();
    if (!init) {
        throw std::runtime_error("Failed to initialize DataWriter: " + init.error());
    }
}

DataWriter::DataWriter(Create, std::unique_ptr<IStorageBackend>&& backend) : backend_(std::move(backend)) {
    auto init = [this]() -> std::expected<void, std::string> {
        if (auto res = file_header_.read(*backend_); !res) return res;

        if (file_header_.internal_metadata().empty()) {
            return std::unexpected("Cannot append: internal metadata is missing from the file header.");
        }
        auto decompressed_res = get_zstd_compressor().decompress(file_header_.internal_metadata());
        if (!decompressed_res) return std::unexpected("Failed to decompress internal metadata: " + decompressed_res.error());

        storage::MemoryBackend temp_meta_backend;
        if (auto res = temp_meta_backend.write(*decompressed_res); !res) return std::unexpected(res.error());
        if (auto res = temp_meta_backend.rewind(); !res) return res;

        auto capacity_res = serialization::read_pod<uint64_t>(temp_meta_backend);
        if (!capacity_res) return std::unexpected("Failed to read chunk offsets block capacity: " + capacity_res.error());
        chunk_offsets_block_capacity_ = *capacity_res;

        auto tell_res = backend_->tell();
        if (!tell_res) return std::unexpected(tell_res.error());
        uint64_t current_block_file_offset = *tell_res;

        while (current_block_file_offset != 0) {
            if (auto seek_res = backend_->seek(current_block_file_offset); !seek_res) {
                return std::unexpected(seek_res.error());
            }

            auto size_res = serialization::read_pod<uint32_t>(*backend_);
            if (!size_res) return std::unexpected("Failed to read block size: " + size_res.error());
            const uint32_t block_size_on_disk = *size_res;

            auto type_res = serialization::read_pod<ChunkOffsetType>(*backend_);
            if (!type_res) return std::unexpected("Failed to read block type: " + type_res.error());
            const auto block_type = *type_res;

            auto hash_res = serialization::read_pod<blake3_hash256_t>(*backend_);
            if (!hash_res) return std::unexpected("Failed to read block hash: " + hash_res.error());
            const auto block_hash = *hash_res;

            auto next_offset_res = serialization::read_pod<uint64_t>(*backend_);
            if (!next_offset_res) return std::unexpected("Failed to read next block offset: " + next_offset_res.error());
            
            memory::vector<uint64_t> offsets;

            if (block_type == ChunkOffsetType::RAW) {
                auto payload_res = serialization::read_vector_pod<uint64_t>(*backend_);
                if (!payload_res) return std::unexpected("Failed to read RAW block payload: " + payload_res.error());
                offsets = std::move(*payload_res);
            } else if (block_type == ChunkOffsetType::ZSTD_COMPRESSED) {
                auto compressed_blob_res = serialization::read_blob(*backend_);
                if (!compressed_blob_res) return std::unexpected("Failed to read ZSTD blob: " + compressed_blob_res.error());

                auto decompressed_res = get_zstd_compressor().decompress(*compressed_blob_res);
                if (!decompressed_res) return std::unexpected("Failed to decompress ZSTD block: " + decompressed_res.error());
                
                auto payload_res = serialization::deserialize_vector_pod_from_buffer<uint64_t>(*decompressed_res);
                if (!payload_res) return std::unexpected("Failed to parse decompressed ZSTD payload: " + payload_res.error());
                offsets = std::move(*payload_res);
            } else {
                return std::unexpected("Unknown ChunkOffsetsBlock type.");
            }

            ChunkOffsetsBlock block;
            block.set_size(block_size_on_disk);
            block.set_type(block_type);
            block.set_hash(block_hash);
            block.set_next_index_offset(*next_offset_res);
            block.set_offsets(std::move(offsets));

            if (block.get_next_index_offset() == 0) {
                current_chunk_offset_block_start_ = current_block_file_offset;
            }
            current_block_file_offset = block.get_next_index_offset();
            chunk_offset_blocks_.push_back(std::move(block));
        }
        if (chunk_offset_blocks_.empty()) {
            return std::unexpected("Existing file has no chunk offset blocks.");
        }

        current_chunk_offset_block_index_ = 0;
        bool found_empty_slot = false;
        const auto& last_block_offsets = chunk_offset_blocks_.back().offsets();
        for (size_t i = 0; i < chunk_offset_blocks_.back().capacity(); ++i) {
            if (last_block_offsets[i] == 0) {
                current_chunk_offset_block_index_ = i;
                found_empty_slot = true;
                break;
            }
        }
        if (!found_empty_slot) {
            current_chunk_offset_block_index_ = chunk_offsets_block_capacity_;
        }

        auto size_res = backend_->size();
        if (!size_res) return std::unexpected(size_res.error());
        return backend_->seek(*size_res);
    }();
    if (!init) {
        throw std::runtime_error("Failed to open DataWriter for append: " + init.error());
    }
}

namespace {
    class ChunkDataGuard {
        Chunk& source_chunk_;
        memory::vector<std::byte> data_;
        bool released_ = false;

    public:
        explicit ChunkDataGuard(Chunk& source_chunk) : source_chunk_(source_chunk), data_(source_chunk.movable_data()) {}
        ~ChunkDataGuard() {
            if (!released_) {
                source_chunk_.set_data(std::move(data_));
            }
        }
        ChunkDataGuard(const ChunkDataGuard&) = delete;
        ChunkDataGuard& operator=(const ChunkDataGuard&) = delete;
        ChunkDataGuard(ChunkDataGuard&&) = delete;
        ChunkDataGuard& operator=(ChunkDataGuard&&) = delete;

        memory::vector<std::byte>&& release() { released_ = true; return std::move(data_); }
    };
}

std::expected<size_t, std::string> DataWriter::append_chunk(ChunkDataType type, DType dtype, ChunkFlags flags,
                                                          std::span<const int64_t> shape,
                                                          Chunk& source_chunk,
                                                          blake3_hash256_t raw_data_hash) {
    if (shape.size() > MAX_SHAPE_DIMENSIONS) {
        return std::unexpected("Shape has an excessive number of dimensions.");
    }

    for (const auto dim : shape) {
        if (dim < 0) {
            return std::unexpected("Shape dimensions cannot be negative.");
        }
    }

    const size_t new_chunk_index = num_chunks();

    if (current_chunk_offset_block_index_ >= chunk_offsets_block_capacity_) {
        uint64_t previous_block_offset = current_chunk_offset_block_start_;
        if (auto res = write_new_chunk_offsets_block(previous_block_offset); !res) {
            return std::unexpected(res.error());
        }
    }

    ChunkDataGuard data_guard(source_chunk);

    Chunk chunk;
    chunk.set_type(type);
    chunk.set_dtype(dtype);
    chunk.set_hash(raw_data_hash);
    chunk.set_flags(flags);

    memory::vector<int64_t> shape_vec;
    if (shape.empty() || shape.back() != 0) {
        shape_vec.reserve(shape.size() + 1);
        shape_vec.assign(shape.begin(), shape.end());
        shape_vec.push_back(0);
    } else {
        shape_vec.assign(shape.begin(), shape.end());
    }
    chunk.set_shape(std::move(shape_vec));
    chunk.set_data(data_guard.release());

    size_t calculated_chunk_size_raw =
        sizeof(uint32_t) + // size
        sizeof(uint16_t) + // type
        sizeof(uint16_t) + // dtype
        sizeof(blake3_hash256_t) + // hash
        sizeof(uint64_t) + // flags
        sizeof(uint32_t) + (chunk.shape().size() * sizeof(int64_t)) + // shape
        sizeof(uint32_t) + chunk.data().size(); // data

    if (calculated_chunk_size_raw > std::numeric_limits<uint32_t>::max()) {
        return std::unexpected("Calculated chunk size exceeds maximum for uint32_t.");
    }
    chunk.set_size(static_cast<uint32_t>(calculated_chunk_size_raw));

    auto tell_res = backend_->tell();
    if (!tell_res) return std::unexpected(tell_res.error());
    uint64_t chunk_start_offset = *tell_res;

    if (auto res = chunk.write(*backend_); !res) return std::unexpected(res.error());

    auto end_tell_res = backend_->tell();
    if (!end_tell_res) return std::unexpected(end_tell_res.error());
    const uint64_t end_of_chunk_pos = *end_tell_res;
    
    if (auto res = backend_->flush(); !res) return std::unexpected(res.error());

    auto& current_block = chunk_offset_blocks_.back();
    auto offsets = current_block.offsets();
    offsets[current_chunk_offset_block_index_] = chunk_start_offset;
    current_block.set_offsets(std::move(offsets));

    const auto raw_payload_bytes_for_hash = serialization::serialize_vector_pod_to_buffer(
        std::span(current_block.offsets()));
    Blake3StreamHasher recalculator;
    recalculator.update(std::span<const std::byte>(raw_payload_bytes_for_hash));
    current_block.set_hash(recalculator.finalize_256());

    const uint64_t base_header_size = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(blake3_hash256_t) + sizeof(uint64_t);
    const uint64_t offset_in_block = base_header_size + sizeof(uint32_t) + (current_chunk_offset_block_index_ * sizeof(uint64_t));
    if (auto res = serialization::write_pod_at(*backend_, current_chunk_offset_block_start_ + offset_in_block,
                                                 chunk_start_offset); !res)
        return std::unexpected(res.error());

    const uint64_t hash_offset_in_block = sizeof(uint32_t) + sizeof(uint16_t);
    if (auto res = serialization::write_pod_at(*backend_, current_chunk_offset_block_start_ + hash_offset_in_block,
                                                 current_block.hash()); !res)
        return std::unexpected(res.error());

    if (auto res = backend_->seek(end_of_chunk_pos); !res) return std::unexpected(res.error());

    current_chunk_offset_block_index_++;
    return new_chunk_index;
}

std::expected<void, std::string> DataWriter::set_user_metadata(std::span<const std::byte> user_metadata) {
    if (num_chunks() > 0) {
        return std::unexpected("User metadata can only be set on a new, empty file before any chunks are written.");
    }
    
    if (auto res = backend_->rewind(); !res) return res;

    auto& compressor = get_zstd_compressor();
    auto compressed_meta_res = compressor.compress(user_metadata);
    if (!compressed_meta_res) {
        return std::unexpected("Failed to compress user metadata: " + compressed_meta_res.error());
    }
    file_header_.set_user_metadata(std::move(*compressed_meta_res));

    if (auto res = file_header_.write(*backend_); !res) return res;

    chunk_offset_blocks_.clear();
    return write_new_chunk_offsets_block(0);
}

std::expected<void, std::string> DataWriter::flush() {
    return backend_->flush();
}

[[nodiscard]] std::expected<std::unique_ptr<storage::IStorageBackend>, std::string> DataWriter::release_backend() {
    if (auto res = flush(); !res) {
        backend_.reset();
        return std::unexpected(res.error());
    }
    return std::move(backend_);
}

[[nodiscard]] size_t DataWriter::num_chunks() const {
    if (chunk_offset_blocks_.empty()) {
        return 0;
    }
    size_t total_chunks = (chunk_offset_blocks_.size() - 1) * chunk_offsets_block_capacity_;
    total_chunks += current_chunk_offset_block_index_;
    return total_chunks;
}

} // namespace cryptodd