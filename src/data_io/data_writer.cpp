#include "data_writer.h"

#include "data_reader.h"

#include <filesystem> // For std::filesystem::exists
#include <span>

namespace cryptodd {
    
// Helper to compress data using Zstd
std::vector<uint8_t> DataWriter::compress_zstd(std::span<const uint8_t> input) {
    if (input.empty()) {
        return {};
    }
    size_t const cBuffSize = ZSTD_compressBound(input.size());
    std::vector<uint8_t> compressed_data(cBuffSize);
    size_t const compressed_size = ZSTD_compress(compressed_data.data(), cBuffSize, input.data(), input.size(), 1); // Compression level 1
    if (ZSTD_isError(compressed_size)) {
        throw std::runtime_error("Zstd compression failed: " + std::string(ZSTD_getErrorName(compressed_size)));
    }
    compressed_data.resize(compressed_size);
    return compressed_data;
}

void DataWriter::write_new_chunk_offsets_block(uint64_t previous_block_offset) {
    // Create a new ChunkOffsetsBlock
    ChunkOffsetsBlock new_block;
    new_block.type = ChunkOffsetType::RAW; // For now, always raw
    new_block.offsets_and_pointer.resize(chunk_offsets_block_capacity_ + 1, 0); // +1 for the next_index_offset pointer

    // Record the start position of this new block
    uint64_t new_block_start_pos = backend_->tell();
    current_chunk_offset_block_start_ = new_block_start_pos;

    // We hash the contents as they are written (all zeros initially).
    Blake3StreamHasher initial_hasher;
    initial_hasher.update(std::span<const uint64_t>(new_block.offsets_and_pointer));
    new_block.hash = initial_hasher.finalize_128();

    // Calculate the actual size of the block.
    new_block.size = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(blake3_hash128_t) +
                     sizeof(uint32_t) + // for the vector's size prefix
                     (new_block.offsets_and_pointer.size() * sizeof(uint64_t));

    // Write the entire block to the backend.
    new_block.write(*backend_);

    chunk_offset_blocks_.push_back(new_block);
    current_chunk_offset_block_index_ = 0;

    // Update the previous block's next_index_offset if applicable
    if (previous_block_offset != 0) {
        update_previous_chunk_offsets_block_pointer(previous_block_offset, new_block_start_pos);
    }
}

void DataWriter::update_previous_chunk_offsets_block_pointer(uint64_t previous_block_offset, uint64_t new_block_offset) {
    // Store current position to restore it later
    const uint64_t original_pos = backend_->tell();

    if (chunk_offset_blocks_.size() < 2) {
        throw std::runtime_error("Attempted to update previous block pointer when no previous block exists.");
    }
    
    // 1. Update the in-memory representation of the previous block.
    ChunkOffsetsBlock& prev_block = chunk_offset_blocks_[chunk_offset_blocks_.size() - 2];
    prev_block.set_next_index_offset(new_block_offset);

    // 2. RECALCULATE and update the hash for the previous block.
    Blake3StreamHasher recalculator;
    recalculator.update(std::span<const uint64_t>(prev_block.offsets_and_pointer));
    prev_block.hash = recalculator.finalize_128();

    // 3. Update the hash on disk.
    const uint64_t hash_offset_in_block = previous_block_offset + sizeof(uint32_t) + sizeof(uint16_t);
    write_pod_at(*backend_, hash_offset_in_block, prev_block.hash);

    // 4. Update the pointer on disk.
    const uint64_t pointer_offset = previous_block_offset + prev_block.size - sizeof(uint64_t);
    write_pod_at(*backend_, pointer_offset, new_block_offset);
    
    // Restore original position
    backend_->seek(original_pos);
}

DataWriter DataWriter::create_new(const std::filesystem::path& filepath, size_t chunk_offsets_block_capacity,
                                  std::span<const uint8_t> user_metadata) {
    return DataWriter(filepath, chunk_offsets_block_capacity, user_metadata);
}

DataWriter DataWriter::open_for_append(const std::filesystem::path& filepath) {
    return DataWriter(filepath, for_append);
}

DataWriter DataWriter::create_in_memory(size_t chunk_offsets_block_capacity, std::span<const uint8_t> user_metadata) {
    auto backend = std::make_unique<MemoryBackend>();
    return DataWriter(std::move(backend), chunk_offsets_block_capacity, user_metadata);
}


DataWriter::DataWriter(const std::filesystem::path& filepath, const size_t chunk_offsets_block_capacity,
                       const std::span<const uint8_t> user_metadata)
    : DataWriter(std::make_unique<FileBackend>(filepath, std::ios_base::out | std::ios_base::binary | std::ios_base::trunc), // NOLINT(*-pass-by-value)
                 chunk_offsets_block_capacity, user_metadata) {
    if (std::filesystem::exists(filepath)) {
        throw std::runtime_error("File already exists: " + filepath.string() + ". Use append constructor for existing files.");
    }
}

DataWriter::DataWriter(const std::filesystem::path& filepath, for_append_t)
    : DataWriter(std::make_unique<FileBackend>(filepath, std::ios_base::in | std::ios_base::out | std::ios_base::binary)) {
    if (!std::filesystem::exists(filepath)) {
        throw std::runtime_error("File does not exist: " + filepath.string() + ". Use new file constructor for new files.");
    }
}

DataWriter::DataWriter(std::unique_ptr<IStorageBackend> backend, size_t chunk_offsets_block_capacity,
                       std::span<const uint8_t> user_metadata)
    : backend_(std::move(backend)), chunk_offsets_block_capacity_(chunk_offsets_block_capacity) { // NOLINT(*-pass-by-value)
    // Prepare internal metadata
    InternalMetadata internal_meta{};
    internal_meta.chunk_offsets_block_capacity = chunk_offsets_block_capacity_;
    
    // Serialize internal metadata to a temporary in-memory backend
    MemoryBackend temp_meta_backend;
    internal_meta.write(temp_meta_backend);
    temp_meta_backend.rewind();
    std::vector<uint8_t> serialized_internal_meta(temp_meta_backend.size());
    temp_meta_backend.read(serialized_internal_meta);

    // Compress user metadata
    file_header_.user_metadata = compress_zstd(user_metadata);
    // Compress internal metadata
    file_header_.internal_metadata = compress_zstd(serialized_internal_meta);

    file_header_.write(*backend_);

    // Write the first ChunkOffsetsBlock
    write_new_chunk_offsets_block(0); // No previous block
}

DataWriter::DataWriter(std::unique_ptr<IStorageBackend> backend) : backend_(std::move(backend)) {
    // Read FileHeader
    file_header_.read(*backend_);

    // Decompress and read internal metadata to configure the writer
    if (file_header_.internal_metadata.empty()) {
        throw std::runtime_error("Cannot append: internal metadata is missing from the file header.");
    }
    size_t decompressed_size = ZSTD_getFrameContentSize(file_header_.internal_metadata.data(), file_header_.internal_metadata.size());
    if (decompressed_size == ZSTD_CONTENTSIZE_ERROR || decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        throw std::runtime_error("Cannot determine decompressed size of internal metadata.");
    }
    std::vector<uint8_t> decompressed_internal_meta = DataReader::decompress_zstd(file_header_.internal_metadata, decompressed_size);
    MemoryBackend temp_meta_backend;
    temp_meta_backend.write(decompressed_internal_meta);
    temp_meta_backend.rewind();
    InternalMetadata internal_meta{};
    internal_meta.read(temp_meta_backend);
    chunk_offsets_block_capacity_ = internal_meta.chunk_offsets_block_capacity;

    // Read all ChunkOffsetsBlocks to build the in-memory index
    uint64_t header_end_pos = backend_->tell(); // Position after FileHeader
    uint64_t current_block_file_offset = header_end_pos;
    while (current_block_file_offset != 0) {
        backend_->seek(current_block_file_offset);
        ChunkOffsetsBlock block;
        block.read(*backend_);
        chunk_offset_blocks_.push_back(block);
        
        // This is needed to correctly set current_chunk_offset_block_start_
        if (block.get_next_index_offset() == 0) {
            current_chunk_offset_block_start_ = current_block_file_offset;
        }
        current_block_file_offset = block.get_next_index_offset();
    }
    if (chunk_offset_blocks_.empty()) {
        throw std::runtime_error("Existing file has no chunk offset blocks.");
    }

    // Set current_chunk_offset_block_index_ to the first empty slot in the last block
    current_chunk_offset_block_index_ = 0; // Default to 0
    bool found_empty_slot = false;
    for (size_t i = 0; i < chunk_offset_blocks_.back().capacity(); ++i) {
        if (chunk_offset_blocks_.back().offsets_and_pointer[i] == 0) {
            current_chunk_offset_block_index_ = i;
            found_empty_slot = true;
            break;
        }
    }
    if (!found_empty_slot) {
        current_chunk_offset_block_index_ = chunk_offsets_block_capacity_; // Block is full
    }
    
    // After loading existing blocks, seek to the end of the file to prepare for appending.
    backend_->seek(backend_->size());
}

void DataWriter::append_chunk(ChunkDataType type, DType dtype, uint64_t flags,
                              std::span<const uint32_t> shape, std::span<const uint8_t> data) {
    if (shape.size() > MAX_SHAPE_DIMENSIONS) {
        throw std::invalid_argument("Shape has an excessive number of dimensions (> " + std::to_string(MAX_SHAPE_DIMENSIONS) + ").");
    }

    if (current_chunk_offset_block_index_ >= chunk_offsets_block_capacity_) {
        uint64_t previous_block_offset = current_chunk_offset_block_start_;
        write_new_chunk_offsets_block(previous_block_offset);
    }

    std::vector<uint8_t> processed_data;
    if (type == ChunkDataType::ZSTD_COMPRESSED) {
        processed_data = compress_zstd(data);
    } else {
        processed_data.assign(data.begin(), data.end());
    }

    Chunk chunk;
    chunk.type = type;
    chunk.dtype = dtype;
    chunk.hash = calculate_blake3_hash128(data);
    chunk.flags = flags;
    if (shape.empty() || shape.back() != 0) {
        chunk.shape.reserve(shape.size() + 1);
        chunk.shape.assign(shape.begin(), shape.end());
        chunk.shape.push_back(0);
    } else {
        chunk.shape.assign(shape.begin(), shape.end());
    }
    chunk.data = std::move(processed_data);

    size_t calculated_chunk_size_raw = 
        sizeof(chunk.size) + 
        sizeof(static_cast<uint16_t>(chunk.type)) + 
        sizeof(static_cast<uint16_t>(chunk.dtype)) + 
        sizeof(chunk.hash) + 
        sizeof(chunk.flags) + 
        sizeof(uint32_t) + (chunk.shape.size() * sizeof(uint32_t)) +
        sizeof(uint32_t) + chunk.data.size();

    if (calculated_chunk_size_raw > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Calculated chunk size exceeds maximum for uint32_t.");
    }
    chunk.size = static_cast<uint32_t>(calculated_chunk_size_raw);

    uint64_t chunk_start_offset = backend_->tell();
    chunk.write(*backend_);

    // FIX 1: Save the file position after writing the chunk.
    const uint64_t end_of_chunk_pos = backend_->tell();

    // --- Update the ChunkOffsetsBlock on disk ---
    auto& current_block = chunk_offset_blocks_.back();

    current_block.offsets_and_pointer[current_chunk_offset_block_index_] = chunk_start_offset;
    
    // FIX 2: Re-calculate the hash of the ENTIRE updated offsets block.
    Blake3StreamHasher recalculator;
    recalculator.update(std::span<const uint64_t>(current_block.offsets_and_pointer));
    current_block.hash = recalculator.finalize_128();

    // Perform two small, targeted writes to update the disk.
    // a) Write the new chunk offset to its slot.
    const uint64_t offset_in_block = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(blake3_hash128_t) + sizeof(uint32_t) + (current_chunk_offset_block_index_ * sizeof(uint64_t));
    write_pod_at(*backend_, current_chunk_offset_block_start_ + offset_in_block, chunk_start_offset);
    
    // b) Write the new hash to the block's header.
    const uint64_t hash_offset_in_block = sizeof(uint32_t) + sizeof(uint16_t);
    write_pod_at(*backend_, current_chunk_offset_block_start_ + hash_offset_in_block, current_block.hash);

    // FIX 1 (cont.): Restore the file pointer to the end of the chunk.
    backend_->seek(end_of_chunk_pos);

    current_chunk_offset_block_index_++;
}

void DataWriter::flush() {
    backend_->flush();
}

std::unique_ptr<IStorageBackend> DataWriter::release_backend() {
    flush(); // Ensure everything is written before releasing.
    return std::move(backend_);
}

size_t DataWriter::num_chunks() const {
    if (chunk_offset_blocks_.empty()) {
        return 0;
    }
    size_t total_chunks = (chunk_offset_blocks_.size() - 1) * chunk_offsets_block_capacity_;
    total_chunks += current_chunk_offset_block_index_;
    return total_chunks;
}

} // namespace cryptodd
