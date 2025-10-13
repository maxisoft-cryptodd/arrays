#include "blake3_stream_hasher.h"
#include <stdexcept>
#include <algorithm>
#include <blake3.h>
#include <mutex>

namespace cryptodd {

struct Blake3StreamHasher::Impl {
    blake3_hasher hasher_{};
    mutable std::once_flag init_flag_{};
    bool is_initialized_ = false; // To track state for finalization checks
};

Blake3StreamHasher::Blake3StreamHasher() : pimpl_(std::make_unique<Impl>()) {}

Blake3StreamHasher::~Blake3StreamHasher() = default;

Blake3StreamHasher::Blake3StreamHasher(Blake3StreamHasher&&) noexcept = default;

Blake3StreamHasher& Blake3StreamHasher::operator=(Blake3StreamHasher&&) noexcept = default;

void Blake3StreamHasher::reset() {
    pimpl_ = std::make_unique<Impl>();
}

void Blake3StreamHasher::init() {
    blake3_hasher_init(&pimpl_->hasher_);
    pimpl_->is_initialized_ = true;
}

void Blake3StreamHasher::update_bytes(std::span<const std::byte> data) {
    std::call_once(pimpl_->init_flag_, [this] { init(); });
    blake3_hasher_update(&pimpl_->hasher_, data.data(), data.size());
}

void Blake3StreamHasher::finalize_to_span(std::span<std::byte> out) const {
    if (!pimpl_->is_initialized_) {
        // This check is for cases where finalize is called without any updates.
        throw std::logic_error("Hasher has not been initialized before finalizing.");
    }
    blake3_hasher_finalize(&pimpl_->hasher_, reinterpret_cast<uint8_t*>(out.data()), out.size());
}

memory::vector<std::byte> Blake3StreamHasher::finalize(size_t out_len) const {
    memory::vector<std::byte> hash_bytes(out_len);
    finalize_to_span(hash_bytes);
    return hash_bytes;
}

blake3_hash128_t Blake3StreamHasher::finalize_128() const {
    if (!pimpl_->is_initialized_) {
        throw std::logic_error("Hasher has not been initialized before finalizing.");
    }
    blake3_hash128_t hash_u64{};
    blake3_hasher_finalize(&pimpl_->hasher_, reinterpret_cast<uint8_t*>(&hash_u64), sizeof(hash_u64));
    return hash_u64;
}

} // namespace cryptodd