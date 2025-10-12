#include "file_backend.h"
#include <limits>

namespace cryptodd::storage {

FileBackend::FileBackend(const std::filesystem::path& filepath, std::ios_base::openmode mode)
    : filepath_(filepath) {
    file_.open(filepath_, mode);
    if (!file_.is_open()) {
        throw std::runtime_error("Failed to open file: " + filepath_.string());
    }
}

FileBackend::~FileBackend() {
    if (file_.is_open()) {
        file_.close();
    }
}

std::expected<size_t, std::string> FileBackend::read(std::span<std::byte> buffer) {
    if (file_.fail() || file_.bad()) {
        return std::unexpected("File stream is in a bad state before read operation.");
    }
    if (buffer.size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
        return std::unexpected("Read buffer size is too large for fstream operation.");
    }
    file_.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    if (file_.bad()) {
        return std::unexpected("File stream is in a bad state after read operation.");
    }
    return static_cast<size_t>(file_.gcount());
}

std::expected<size_t, std::string> FileBackend::write(std::span<const std::byte> data) {
    if (file_.fail() || file_.bad()) {
        return std::unexpected("File stream is in a bad state before write operation.");
    }
    if (data.size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
        return std::unexpected("Write data size is too large for fstream operation.");
    }
    file_.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file_.good()) {
        return std::unexpected("Failed to write data to file.");
    }
    return data.size();
}

std::expected<void, std::string> FileBackend::seek(uint64_t offset) {
    if (offset > static_cast<uint64_t>(std::numeric_limits<off_type>::max())) {
        return std::unexpected("File offset is too large for fstream.");
    }
    auto signed_offset = static_cast<off_type>(offset);
    file_.seekg(signed_offset);
    file_.seekp(signed_offset);
    if (!file_.good()) {
        return std::unexpected("Failed to seek to offset.");
    }
    return {};
}

std::expected<uint64_t, std::string> FileBackend::tell() {
    auto pos = file_.tellg();
    if (pos == -1) {
        return std::unexpected("Failed to get current file position.");
    }
    return static_cast<uint64_t>(pos);
}

std::expected<void, std::string> FileBackend::flush() {
    file_.flush();
    if (!file_.good()) {
        return std::unexpected("Failed to flush file stream.");
    }
    return {};
}

std::expected<void, std::string> FileBackend::rewind() {
    file_.clear(); // Clear any error flags (like EOF)
    file_.seekg(0);
    file_.seekp(0);
    if (!file_.good()) {
        return std::unexpected("Failed to rewind file stream.");
    }
    return {};
}

std::expected<uint64_t, std::string> FileBackend::size() const {
    std::ifstream in(filepath_, std::ios::binary | std::ios::ate);
    if (!in) {
        return std::unexpected("Failed to open file to get size.");
    }
    return static_cast<uint64_t>(in.tellg());
}

} // namespace cryptodd::storage