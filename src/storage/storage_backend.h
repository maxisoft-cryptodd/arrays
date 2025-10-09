#pragma once

#include <cstdint>
#include <utility>
#include <vector>
#include <string>
#include <fstream>
#include <stdexcept>
#include <filesystem>
#include <limits>
#include <span>

namespace cryptodd {

// Interface for storage backends
class IStorageBackend {
public:
    virtual ~IStorageBackend() = default;

    // Reads data from the current position into the buffer.
    // Returns the number of bytes read.
    virtual size_t read(std::span<uint8_t> buffer) = 0;

    // Writes data from the buffer to the current position.
    // Returns the number of bytes written.
    virtual size_t write(std::span<const uint8_t> data) = 0;

    // Seeks to the specified offset.
    virtual void seek(uint64_t offset) = 0;

    // Returns the current position in the stream.
    virtual uint64_t tell() = 0;

    // Flushes any buffered writes to the underlying storage.
    virtual void flush() = 0;

    // Resets the current position to the beginning of the storage.
    virtual void rewind() = 0;

    // Returns the total size of the storage.
    [[nodiscard]] virtual uint64_t size() const = 0;
};

// File-based storage backend
class FileBackend : public IStorageBackend {
private:
    using off_type = std::fstream::off_type;

    std::fstream file_;
    std::filesystem::path filepath_;

public:
    explicit FileBackend(std::filesystem::path filepath, std::ios_base::openmode mode = std::ios_base::in | std::ios_base::out | std::ios_base::binary)
        : filepath_(std::move(filepath)) {
        file_.open(filepath_, mode);
        if (!file_.is_open()) {
            throw std::runtime_error("Failed to open file: " + filepath_.string());
        }
    }

    ~FileBackend() override {
        if (file_.is_open()) {
            file_.close();
        }
    }

    size_t read(std::span<uint8_t> buffer) override {
        if (file_.fail() || file_.bad()) {
            throw std::runtime_error("File stream is in a bad state before read operation.");
        }
        if (buffer.size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
            throw std::out_of_range("Read buffer size " + std::to_string(buffer.size()) + " is too large for fstream operation.");
        }
        file_.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        return file_.gcount();
    }

    size_t write(std::span<const uint8_t> data) override {
        if (file_.fail() || file_.bad()) {
            throw std::runtime_error("File stream is in a bad state before write operation.");
        }
        if (data.size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
            throw std::out_of_range("Write data size " + std::to_string(data.size()) + " is too large for fstream operation.");
        }
        file_.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!file_.good()) {
            throw std::runtime_error("Failed to write data to file.");
        }
        return data.size();
    }

    void seek(uint64_t offset) override {
        if (offset > static_cast<uint64_t>(std::numeric_limits<off_type>::max())) {
            throw std::out_of_range("File offset " + std::to_string(offset) + " is too large for fstream.");
        }
        auto signed_offset = static_cast<off_type>(offset);
        file_.seekg(signed_offset);
        file_.seekp(signed_offset);
        if (!file_.good()) {
            throw std::runtime_error("Failed to seek to offset " + std::to_string(offset));
        }
    }

    uint64_t tell() override {
        return file_.tellg();
    }

    void flush() override {
        file_.flush();
    }

    void rewind() override {
        file_.clear(); // Clear any error flags (like EOF)
        file_.seekg(0);
        file_.seekp(0);
    }

    [[nodiscard]] uint64_t size() const override {
        std::ifstream in(filepath_, std::ios::binary | std::ios::ate); // std::ifstream can take a path object
        return in.tellg();
    }
};

// In-memory storage backend
class MemoryBackend : public IStorageBackend {
private:
    std::vector<uint8_t> buffer_;
    uint64_t current_pos_ = 0;

public:
    explicit MemoryBackend(size_t initial_capacity = 0) {
        buffer_.reserve(initial_capacity);
    }

    size_t read(std::span<uint8_t> buffer) override {
        if (current_pos_ >= buffer_.size()) {
            return 0; // EOF
        }
        const size_t actual_bytes_to_read = std::min(buffer.size(), buffer_.size() - current_pos_);
        if (current_pos_ > static_cast<decltype(current_pos_)>(std::numeric_limits<std::vector<uint8_t>::iterator::difference_type>::max())) {
            throw std::out_of_range("Memory offset " + std::to_string(current_pos_) + " is too large.");
        }
        std::copy_n(buffer_.cbegin() + static_cast<ptrdiff_t>(current_pos_), actual_bytes_to_read, buffer.begin());
        current_pos_ += actual_bytes_to_read;
        return actual_bytes_to_read;
    }

    size_t write(std::span<const uint8_t> data) override {
        if (current_pos_ + data.size() > buffer_.size()) {
            buffer_.resize(current_pos_ + data.size());
        }
        if (current_pos_ > static_cast<decltype(current_pos_)>(std::numeric_limits<std::vector<uint8_t>::iterator::difference_type>::max())) {
            throw std::out_of_range("Memory offset " + std::to_string(current_pos_) + " is too large.");
        }
        std::ranges::copy(data, buffer_.begin() + static_cast<ptrdiff_t>(current_pos_));
        current_pos_ += data.size();
        return data.size();
    }

    void seek(const uint64_t offset) override {
        if (offset > buffer_.size()) {
            // Optionally extend buffer or throw error
            buffer_.resize(offset);
        }
        current_pos_ = offset;
    }

    uint64_t tell() override {
        return current_pos_;
    }

    void flush() override {
        // No-op for in-memory backend
    }

    void rewind() override {
        current_pos_ = 0;
    }

    [[nodiscard]] uint64_t size() const override {
        return buffer_.size();
    }
};

} // namespace cryptodd
