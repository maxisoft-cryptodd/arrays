#include "cdd_file_format.h"
#include "serialization_helpers.h"

namespace cryptodd {
	
using namespace serialization;

// --- FileHeader ---

std::expected<void, std::string> FileHeader::write(IStorageBackend& backend) const {
    size_t bytes_written = 0;
    if (auto res = write_pod(backend, magic_); res) bytes_written += *res; else return std::unexpected(res.error());
    if (auto res = write_pod(backend, version_); res) bytes_written += *res; else return std::unexpected(res.error());
    if (auto res = write_blob(backend, internal_metadata_); res) bytes_written += *res; else return std::unexpected(res.error());
    if (auto res = write_blob(backend, user_metadata_); res) bytes_written += *res; else return std::unexpected(res.error());

    // FileHeader doesn't have an explicit size field, so we just ensure writes succeeded.
    return {};
}

std::expected<void, std::string> FileHeader::read(IStorageBackend& backend) {
    auto magic_res = read_pod<uint32_t>(backend);
    if (!magic_res) return std::unexpected(magic_res.error());
    magic_ = *magic_res;

    auto version_res = read_pod<uint16_t>(backend);
    if (!version_res) return std::unexpected(version_res.error());
    version_ = *version_res;

    auto internal_meta_res = read_blob(backend);
    if (!internal_meta_res) return std::unexpected(internal_meta_res.error());
    internal_metadata_ = std::move(*internal_meta_res);

    auto user_meta_res = read_blob(backend);
    if (!user_meta_res) return std::unexpected(user_meta_res.error());
    user_metadata_ = std::move(*user_meta_res);

    if (magic_ != CDD_MAGIC) {
        return std::unexpected("Invalid CDD file magic.");
    }
    if (version_ != CDD_VERSION) {
        return std::unexpected("Unsupported CDD file version.");
    }
    return {};
}

// --- ChunkOffsetsBlock ---

std::expected<void, std::string> ChunkOffsetsBlock::write(IStorageBackend& backend) const {
    auto start_pos = backend.tell();
    if (!start_pos) return std::unexpected("Failed to get starting position for ChunkOffsetsBlock write: " + start_pos.error());

    size_t bytes_written = 0;
    if (auto res = write_pod(backend, size_); res) bytes_written += *res; else return std::unexpected(res.error());
    if (auto res = write_pod(backend, static_cast<uint16_t>(type_)); res) bytes_written += *res; else return std::unexpected(res.error());
    if (auto res = write_pod(backend, hash_); res) bytes_written += *res; else return std::unexpected(res.error());
    if (auto res = write_vector_pod(backend, std::span(offsets_and_pointer_)); res) bytes_written += *res; else return std::unexpected(res.error());

    // The total bytes written must equal the size field of the block.
    // This is a critical integrity check.
    if (bytes_written != size_) {
        return std::unexpected("ChunkOffsetsBlock size mismatch during write.");
    }
    return {};
}

std::expected<void, std::string> ChunkOffsetsBlock::read(IStorageBackend& backend) {
    auto start_pos = backend.tell();
    if (!start_pos) return std::unexpected(start_pos.error());

    auto size_res = read_pod<uint32_t>(backend);
    if (!size_res) return std::unexpected(size_res.error());
    size_ = *size_res;

    auto type_res = read_pod<uint16_t>(backend);
    if (!type_res) return std::unexpected(type_res.error());
    type_ = static_cast<ChunkOffsetType>(*type_res);

    auto hash_res = read_pod<blake3_hash256_t>(backend);
    if (!hash_res) return std::unexpected(hash_res.error());
    hash_ = *hash_res;

    auto offsets_res = read_vector_pod<uint64_t>(backend);
    if (!offsets_res) return std::unexpected(offsets_res.error());
    offsets_and_pointer_ = std::move(*offsets_res);

    auto end_pos = backend.tell();
    if (!end_pos) return std::unexpected(end_pos.error());
    if (*end_pos - *start_pos != size_) {
        return std::unexpected("ChunkOffsetsBlock size mismatch during read.");
    }
    return {};
}

size_t ChunkOffsetsBlock::capacity() const {
    if (offsets_and_pointer_.empty()) return 0;
    return offsets_and_pointer_.size() - 1;
}

uint64_t ChunkOffsetsBlock::get_next_index_offset() const {
    if (offsets_and_pointer_.empty()) return 0;
    return offsets_and_pointer_.back();
}

void ChunkOffsetsBlock::set_next_index_offset(uint64_t offset) {
    if (offsets_and_pointer_.empty()) {
        throw std::runtime_error("Cannot set next_index_offset on an empty offsets_and_pointer vector.");
    }
    offsets_and_pointer_.back() = offset;
}

// --- Chunk ---

std::expected<void, std::string> Chunk::write(IStorageBackend& backend) const {
    auto start_pos = backend.tell();
    if (!start_pos) return std::unexpected("Failed to get starting position for Chunk write: " + start_pos.error());

    size_t bytes_written = 0;
    if (auto res = write_pod(backend, size_); res) bytes_written += *res; else return std::unexpected(res.error());
    if (auto res = write_pod(backend, static_cast<uint16_t>(type_)); res) bytes_written += *res; else return std::unexpected(res.error());
    if (auto res = write_pod(backend, static_cast<uint16_t>(dtype_)); res) bytes_written += *res; else return std::unexpected(res.error());
    if (auto res = write_pod(backend, hash_); res) bytes_written += *res; else return std::unexpected(res.error());
    if (auto res = write_pod(backend, flags_); res) bytes_written += *res; else return std::unexpected(res.error());
    if (auto res = write_vector_pod(backend, std::span(shape_)); res) bytes_written += *res; else return std::unexpected(res.error());
    if (auto res = write_blob(backend, data_); res) bytes_written += *res; else return std::unexpected(res.error());

    // The total bytes written must equal the size field of the chunk.
    // This is a critical integrity check.
    if (bytes_written != size_) {
        return std::unexpected("Chunk size mismatch during write.");
    }
    return {};
}

std::expected<void, std::string> Chunk::read(IStorageBackend& backend) {
    auto start_pos = backend.tell();
    if (!start_pos) return std::unexpected(start_pos.error());

    if (auto res = read_pod<uint32_t>(backend); res) size_ = *res; else return std::unexpected(res.error());
    if (auto res = read_pod<uint16_t>(backend); res) type_ = static_cast<ChunkDataType>(*res); else return std::unexpected(res.error());
    if (auto res = read_pod<uint16_t>(backend); res) dtype_ = static_cast<DType>(*res); else return std::unexpected(res.error());
    if (auto res = read_pod<blake3_hash256_t>(backend); res) hash_ = *res; else return std::unexpected(res.error());
    if (auto res = read_pod<uint64_t>(backend); res) flags_ = *res; else return std::unexpected(res.error());
    if (auto res = read_vector_pod<int64_t>(backend); res) shape_ = std::move(*res); else return std::unexpected(res.error());
    if (auto res = read_blob(backend); res) data_ = std::move(*res); else return std::unexpected(res.error());

    auto end_pos = backend.tell();
    if (!end_pos) return std::unexpected(end_pos.error());
    if (*end_pos - *start_pos != size_) {
        return std::unexpected("Chunk size mismatch during read.");
    }
    return {};
}

std::span<const int64_t> Chunk::get_shape() const {
    size_t size = shape_.size();
    if (size == 0)
    {
        return {};
    }

    // The shape vector is null-terminated in the file format to handle older versions
    // or different language writers. This getter provides a clean view without the terminator.
    if (shape_.back() == 0)
    {
        size -= 1;
    }

    return {shape_.data(), size};
}

size_t Chunk::num_elements() const {
    size_t res = 1;
    const auto s = get_shape();
    if (s.empty())
    {
        return 0;
    }
    for (auto dim : s)
    {
        if (dim < 0) {
            // A negative dimension means the shape is invalid, so the number of elements is 0.
            return 0;
        }
        res *= static_cast<size_t>(dim);
    }
    return res;
}

} // namespace cryptodd
