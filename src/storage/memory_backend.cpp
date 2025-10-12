#include "memory_backend.h"
#include <algorithm>
#include <limits>

namespace cryptodd::storage {

MemoryBackend::MemoryBackend(size_t initial_capacity) {
    buffer_.reserve(initial_capacity);
}

std::expected<size_t, std::string> MemoryBackend::read(std::span<std::byte> buffer) {
    if (current_pos_ >= buffer_.size()) {
        return 0;
    }
    const size_t actual_bytes_to_read = std::min(buffer.size(), buffer_.size() - current_pos_);
    if (current_pos_ > static_cast<decltype(current_pos_)>(std::numeric_limits<std::vector<std::byte>::iterator::difference_type>::max())) {
        return std::unexpected("Memory offset is too large.");
    }
    std::copy_n(buffer_.cbegin() + static_cast<ptrdiff_t>(current_pos_), actual_bytes_to_read, buffer.data());
    current_pos_ += actual_bytes_to_read;
    return actual_bytes_to_read;
}

std::expected<size_t, std::string> MemoryBackend::write(std::span<const std::byte> data) {
    if (current_pos_ + data.size() > buffer_.size()) {
        buffer_.resize(current_pos_ + data.size());
    }
    if (current_pos_ > static_cast<decltype(current_pos_)>(std::numeric_limits<std::vector<std::byte>::iterator::difference_type>::max())) {
        return std::unexpected("Memory offset is too large.");
    }
    std::copy(data.begin(), data.end(), buffer_.begin() + static_cast<ptrdiff_t>(current_pos_));
    current_pos_ += data.size();
    return data.size();
}

std::expected<void, std::string> MemoryBackend::seek(const uint64_t offset) {
    if (offset > buffer_.size()) {
        // Optionally extend buffer or return an error. Extending is consistent with current behavior.
        buffer_.resize(offset);
    }
    current_pos_ = offset;
    return {};
}

std::expected<uint64_t, std::string> MemoryBackend::tell() {
    return current_pos_;
}

std::expected<void, std::string> MemoryBackend::flush() {
    return {}; // No-op for in-memory backend
}

std::expected<void, std::string> MemoryBackend::rewind() {
    current_pos_ = 0;
    return {};
}

std::expected<uint64_t, std::string> MemoryBackend::size() const {
    return buffer_.size();
}

} // namespace cryptodd::storage