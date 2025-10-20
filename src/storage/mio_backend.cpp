#include "mio_backend.h"

#include <algorithm>
#include <expected>
#include <fstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <variant>

#include <mio/mmap.hpp>

namespace cryptodd::storage {

struct MioBackend::Impl {
    using mmap_source = mio::basic_mmap_source<std::byte>;
    using mmap_sink = mio::basic_mmap_sink<std::byte>;

    // Use a variant to hold either a read-only or a read-write mapping.
    std::variant<std::monostate, mmap_source, mmap_sink> mapping;
};

MioBackend::MioBackend(std::filesystem::path filepath, std::ios_base::openmode mode)
    : pimpl_(std::make_unique<Impl>()), filepath_(std::move(filepath)) {
    writable_ = (mode & std::ios_base::out) != 0;

    std::error_code ec;
    if (writable_) {
        // If writable, create the file if it doesn't exist.
        if (!std::filesystem::exists(filepath_)) {
            // Create an empty file.
            if (std::ofstream file(filepath_, std::ios::binary); !file) {
                throw std::runtime_error("MioBackend: Failed to create file: " + filepath_.string());
            }
        }
    } else {
        // If read-only, the file must exist.
        if (!std::filesystem::exists(filepath_)) {
            throw std::runtime_error("MioBackend: File does not exist for read-only mapping: " + filepath_.string());
        }
    }

    const auto file_size = std::filesystem::file_size(filepath_, ec);
    if (ec) {
        throw std::runtime_error("MioBackend: Failed to get file size: " + filepath_.string() + " - " + ec.message());
    }

    logical_size_ = file_size; // Initialize logical size to the starting file size.

    if (file_size > 0) {
        if (writable_) {
            Impl::mmap_sink sink;
            sink.map(filepath_.native(), ec);
            if (ec) {
                throw std::runtime_error("MioBackend: Failed to create read-write mapping: " + ec.message());
            }
            pimpl_->mapping = std::move(sink);
        } else {
            Impl::mmap_source source;
            source.map(filepath_.native(), ec);
            if (ec) {
                throw std::runtime_error("MioBackend: Failed to create read-only mapping: " + ec.message());
            }
            pimpl_->mapping = std::move(source);
        }
    }
}

MioBackend::~MioBackend() {
    // Ensure data is flushed for writable mappings on destruction.
    // Errors are ignored in the destructor as per RAII best practices.
    if (writable_ && pimpl_ && std::holds_alternative<Impl::mmap_sink>(pimpl_->mapping)) {
        std::error_code ec;
        std::get<Impl::mmap_sink>(pimpl_->mapping).sync(ec);
    }
}

// Implements a smart growth strategy to reduce the frequency of expensive remap operations.
std::expected<void, std::string> MioBackend::remap(uint64_t required_size) {
    auto* sink = std::get_if<Impl::mmap_sink>(&pimpl_->mapping);
    uint64_t current_size = sink ? sink->size() : 0;

    // Smart growth strategy: double the size, but cap the growth increment at 64MB.
    static constexpr uint64_t GROWTH_CAP = 64 * 1024 * 1024; // 64 MB
    uint64_t growth_increment = std::min(current_size, GROWTH_CAP);
    if (growth_increment == 0) growth_increment = 4096; // Start with a page size if file is empty

    uint64_t new_size = std::max(required_size, current_size + growth_increment);

    // Unmap the old view
    if (sink && sink->is_open()) {
        sink->unmap();
    }

    // Resize the underlying file
    std::error_code ec;
    std::filesystem::resize_file(filepath_, new_size, ec);
    if (ec) {
        return std::unexpected("MioBackend: Failed to resize file to " + std::to_string(new_size) + " bytes: " + ec.message());
    }

    // Create a new mapping of the larger file
    Impl::mmap_sink new_sink;
    new_sink.map(filepath_.native(), ec);
    if (ec) {
        return std::unexpected("MioBackend: Failed to remap file after resize: " + ec.message());
    }
    pimpl_->mapping = std::move(new_sink);
    return {};
}

std::expected<size_t, std::string> MioBackend::read(std::span<std::byte> buffer) {
    return std::visit(
        [this, buffer]<typename T0>(T0& map) -> std::expected<size_t, std::string> {
            using T = std::decay_t<T0>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return 0;
            } else {
                if (!map.is_open() || current_pos_ >= logical_size_) { // Read up to logical size
                    return 0;
                }
                const size_t bytes_to_read = std::min(buffer.size(), static_cast<size_t>(logical_size_ - current_pos_));
                std::copy_n(map.data() + current_pos_, bytes_to_read, buffer.data());
                current_pos_ += bytes_to_read;
                return bytes_to_read;
            }
        },
        pimpl_->mapping);
}

std::expected<size_t, std::string> MioBackend::write(std::span<const std::byte> data) {
    if (!writable_) {
        return std::unexpected("MioBackend: Attempted to write to a read-only backend.");
    }
    if (data.empty()) {
        return 0;
    }

    const uint64_t required_size = current_pos_ + data.size();
    uint64_t current_mapped_size = 0;
    if (const auto* sink = std::get_if<Impl::mmap_sink>(&pimpl_->mapping)) {
        current_mapped_size = sink->size();
    }

    if (required_size > current_mapped_size) {
        auto result = remap(required_size);
        if (!result) {
            return std::unexpected(result.error());
        }
    }

    // After remapping, the sink is guaranteed to exist and be large enough.
    auto& sink = std::get<Impl::mmap_sink>(pimpl_->mapping);
    std::ranges::copy(data, sink.data() + current_pos_);
    current_pos_ += data.size();

    // Update the logical size after a successful write.
    logical_size_ = std::max(logical_size_, current_pos_);

    return data.size();
}

std::expected<void, std::string> MioBackend::seek(uint64_t offset) {
    if (writable_) {
        // Eagerly grow if seeking past the current logical size.
        if (offset > logical_size_) {
            uint64_t current_mapped_size = 0;
            if (const auto* sink = std::get_if<Impl::mmap_sink>(&pimpl_->mapping)) {
                current_mapped_size = sink->size();
            }

            // Remap if the physical file is also too small.
            if (offset > current_mapped_size) {
                if (auto* sink = std::get_if<Impl::mmap_sink>(&pimpl_->mapping)) {
                    if (sink->is_open()) {
                        sink->unmap();
                    }
                }
                std::error_code ec;
                std::filesystem::resize_file(filepath_, offset, ec);
                if (ec) {
                    return std::unexpected("MioBackend: Failed to resize file on seek: " + ec.message());
                }
                Impl::mmap_sink new_sink;
                new_sink.map(filepath_.native(), ec);
                if (ec) {
                    return std::unexpected("MioBackend: Failed to remap after seek resize: " + ec.message());
                }
                pimpl_->mapping = std::move(new_sink);
            }
            // Update the logical size to the seek position.
            logical_size_ = offset;
        }
    }

    current_pos_ = offset;
    return {};
}

std::expected<uint64_t, std::string> MioBackend::tell() {
    return current_pos_;
}

std::expected<void, std::string> MioBackend::flush() {
    if (!writable_) {
        return {};
    }

    if (auto* sink = std::get_if<Impl::mmap_sink>(&pimpl_->mapping)) {
        if (sink->is_open()) {
            std::error_code ec;
            sink->sync(ec);
            if (ec) {
                return std::unexpected("MioBackend: Failed to sync mapping: " + ec.message());
            }
        }
    }
    return {};
}

std::expected<void, std::string> MioBackend::rewind() {
    current_pos_ = 0;
    return {};
}

std::expected<uint64_t, std::string> MioBackend::size() {
    // Return the tracked logical size, not the physical file size.
    return logical_size_;
}

}
