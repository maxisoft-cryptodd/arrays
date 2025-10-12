#include "file_backend.h"
#include <limits>

namespace cryptodd::storage {

FileBackend::FileBackend(std::filesystem::path filepath, std::ios_base::openmode mode)
    : filepath_(std::move(filepath)) {

    // If the mode includes writing and the file does not exist,
    // we must create it first. `std::fstream` with `std::ios::in` will not create a file.
    if ((mode & std::ios::out) && !std::filesystem::exists(filepath_)) {
        // Create an empty file by opening and immediately closing an ofstream.
        std::ofstream creator(filepath_, std::ios::binary);
        if (!creator) {
            throw std::runtime_error("FileBackend: Failed to create file: " + filepath_.string());
        }
    }

    if ((mode & std::ios::out) && !(mode & std::ios::in)) {
        mode |= std::ios::trunc;
    }

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

    // --- EAGER RESIZING LOGIC ---
    // Align behavior with MemoryBackend by resizing the file if seeking past the end.
    auto current_size_res = size();
    if (!current_size_res) {
        return std::unexpected("Failed to get current size before seek: " + current_size_res.error());
    }

    if (offset > *current_size_res) {
        // We must resize the file on disk to match the seek offset.
        std::error_code ec;
        std::filesystem::resize_file(filepath_, offset, ec);
        if (ec) {
            return std::unexpected("Failed to resize file on seek: " + ec.message());
        }
    }
    // --- END EAGER RESIZING LOGIC ---

    // Clear any EOF flags that might have been set before seeking.
    file_.clear();
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
        // tellg() can fail if the file is not open or after a failed operation.
        // Let's try to recover by getting the position from tellp().
        pos = file_.tellp();
        if (pos == -1) {
            return std::unexpected("Failed to get current file position from both get and put pointers.");
        }
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
    file_.clear();
    file_.seekg(0);
    file_.seekp(0);
    if (!file_.good()) {
        return std::unexpected("Failed to rewind file stream.");
    }
    return {};
}

std::expected<uint64_t, std::string> FileBackend::size() {
    // Flush any pending writes to ensure the on-disk size is accurate.
    file_.flush();
    if(!file_.good()){
        // If flushing fails, the subsequent size check might be invalid.
        return std::unexpected("File stream in bad state before getting size.");
    }
    
    std::error_code ec;
    const auto file_size = std::filesystem::file_size(filepath_, ec);
    if (ec) {
        return std::unexpected("Failed to get file size: " + ec.message());
    }
    return file_size;
}

}
