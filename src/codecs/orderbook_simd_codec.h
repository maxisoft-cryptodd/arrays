#pragma once

#include <expected>
#include <array>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include "codec_constants.h"
#include "i_compressor.h"

#include <hwy/aligned_allocator.h>
#include <hwy/base.h> // For hwy::float16_t

// Important: never ever include here ! #include <hwy/highway.h>
#ifdef HWY_HIGHWAY_INCLUDED
    static_assert(false, "highway.h is already included !");
#endif


namespace cryptodd {

// Forward declarations for SIMD functions, now in their own namespace
namespace simd
{

void DemoteAndXor_dispatcher(const float* current, const float* prev, hwy::float16_t* out, size_t num_floats);
void ShuffleFloat16_dispatcher(const hwy::float16_t* in, uint8_t* out, size_t num_f16);
void UnshuffleAndReconstruct_dispatcher(const uint8_t* shuffled_in, float* out, size_t num_snapshots, size_t snapshot_floats, std::span<float> last_snapshot_state);

void XorFloat32_dispatcher(const float* current, const float* prev, float* out, size_t num_floats);
void ShuffleFloat32_dispatcher(const float* in, uint8_t* out, size_t num_f32);
void UnshuffleAndReconstructFloat32_dispatcher(const uint8_t* shuffled_in, float* out, size_t num_snapshots, size_t snapshot_floats, std::span<float> last_snapshot_state);

}
class OrderbookSimdCodecWorkspace;

namespace detail {
    // Forward declare implementation functions that need privileged access to the workspace.
    inline std::expected<memory::vector<std::byte>, std::string> encode16_impl(std::span<const float> snapshots, std::span<const float> prev_snapshot,
                                              size_t num_snapshots, size_t snapshot_floats, ICompressor& compressor,
                                              OrderbookSimdCodecWorkspace& workspace);

    inline std::expected<memory::vector<std::byte>, std::string> encode32_impl(std::span<const float> snapshots, std::span<const float> prev_snapshot,
                                              size_t num_snapshots, size_t snapshot_floats, ICompressor& compressor,
                                              OrderbookSimdCodecWorkspace& workspace);
}// namespace detail

/**
 * @brief Manages reusable, SIMD-friendly aligned memory buffers for encoding operations.
 *
 * This class provides a safe, modern C++ interface for managing memory while allowing
 * tightly-coupled implementation details (via `friend`) to access the raw aligned pointers
 * necessary for high-performance SIMD code. It is non-copyable but movable.
 */
class OrderbookSimdCodecWorkspace {
public:
    OrderbookSimdCodecWorkspace() = default;

    // This class manages memory, so it's non-copyable but movable.
    OrderbookSimdCodecWorkspace(const OrderbookSimdCodecWorkspace&) = delete;
    OrderbookSimdCodecWorkspace& operator=(const OrderbookSimdCodecWorkspace&) = delete;
    OrderbookSimdCodecWorkspace(OrderbookSimdCodecWorkspace&&) noexcept = default;
    OrderbookSimdCodecWorkspace& operator=(OrderbookSimdCodecWorkspace&&) noexcept = default;

    /**
     * @brief Ensures the workspace has enough capacity to process a given number of floats.
     * Reallocates memory if the current capacity is insufficient. This can be an expensive operation.
     * @param required_floats The minimum number of floats the workspace must be able to hold.
     */
    void ensure_capacity(const size_t required_floats) {
        if (capacity_in_floats_ >= required_floats) return;
        f16_deltas_ = hwy::AllocateAligned<hwy::float16_t>(required_floats);
        f32_deltas_ = hwy::AllocateAligned<float>(required_floats);
        shuffled_bytes_ = hwy::AllocateAligned<uint8_t>(required_floats * sizeof(float));

        if (!f16_deltas_ || !f32_deltas_ || !shuffled_bytes_) {
            throw std::bad_alloc();
        }

        capacity_in_floats_ = required_floats;
    }

    /**
     * @brief Returns the current capacity of the workspace in number of floats.
     */
    [[nodiscard]] size_t capacity() const noexcept { return capacity_in_floats_; }

    /**
     * @brief Returns a span over the float16 delta buffer.
     */
    [[nodiscard]] std::span<hwy::float16_t> f16_deltas() noexcept {
        return {f16_deltas_.get(), capacity_in_floats_};
    }

    /**
     * @brief Returns a const span over the float16 delta buffer.
     */
    [[nodiscard]] std::span<const hwy::float16_t> f16_deltas() const noexcept {
        return {f16_deltas_.get(), capacity_in_floats_};
    }

    /**
     * @brief Returns a span over the float32 delta buffer.
     */
    [[nodiscard]] std::span<float> f32_deltas() noexcept { return {f32_deltas_.get(), capacity_in_floats_}; }

    /**
     * @brief Returns a const span over the float32 delta buffer.
     */
    [[nodiscard]] std::span<const float> f32_deltas() const noexcept { return {f32_deltas_.get(), capacity_in_floats_}; }

    /**
     * @brief Returns a span over the shuffled bytes buffer.
     */
    [[nodiscard]] std::span<uint8_t> shuffled_bytes() noexcept {
        return {shuffled_bytes_.get(), capacity_in_floats_ * sizeof(float)};
    }

    /**
     * @brief Returns a const span over the shuffled bytes buffer.
     */
    [[nodiscard]] std::span<const uint8_t> shuffled_bytes() const noexcept {
        return {shuffled_bytes_.get(), capacity_in_floats_ * sizeof(float)};
    }

private:
    // Grant access to our internal implementation functions. They need the raw
    // hwy::AlignedFreeUniquePtr to guarantee the alignment contract for SIMD operations.
    friend std::expected<memory::vector<std::byte>, std::string> detail::encode16_impl(std::span<const float>, std::span<const float>, size_t, size_t, ICompressor&, OrderbookSimdCodecWorkspace&);
    friend std::expected<memory::vector<std::byte>, std::string> detail::encode32_impl(std::span<const float>, std::span<const float>, size_t, size_t, ICompressor&, OrderbookSimdCodecWorkspace&);

    hwy::AlignedFreeUniquePtr<hwy::float16_t[]> f16_deltas_;
    hwy::AlignedFreeUniquePtr<float[]> f32_deltas_;
    hwy::AlignedFreeUniquePtr<uint8_t[]> shuffled_bytes_;
    size_t capacity_in_floats_ = 0;
};

namespace detail {

inline std::expected<memory::vector<std::byte>, std::string> encode16_impl(std::span<const float> snapshots, std::span<const float> prev_snapshot,
                                          size_t num_snapshots, size_t snapshot_floats, ICompressor& compressor,
                                          OrderbookSimdCodecWorkspace& workspace) {
    const size_t num_floats = snapshots.size();
    hwy::float16_t* f16_deltas_ptr = workspace.f16_deltas_.get();

    simd::DemoteAndXor_dispatcher(snapshots.data(), prev_snapshot.data(), f16_deltas_ptr, snapshot_floats);
    for (size_t s = 1; s < num_snapshots; ++s) {
        const float* current_snap = snapshots.data() + s * snapshot_floats;
        const float* prev_snap = snapshots.data() + (s - 1) * snapshot_floats;
        hwy::float16_t* out_snap = f16_deltas_ptr + s * snapshot_floats;
        simd::DemoteAndXor_dispatcher(current_snap, prev_snap, out_snap, snapshot_floats);
    }

    const size_t f16_bytes = num_floats * sizeof(hwy::float16_t);
    uint8_t* shuffled_bytes_ptr = workspace.shuffled_bytes().data();

    simd::ShuffleFloat16_dispatcher(f16_deltas_ptr, shuffled_bytes_ptr, num_floats);

    static_assert(sizeof(std::byte) == sizeof(uint8_t));
    std::span<const std::byte> data_to_compress(reinterpret_cast<const std::byte*>(shuffled_bytes_ptr), f16_bytes); // NOLINT
    return compressor.compress(data_to_compress);
}

inline std::expected<memory::vector<std::byte>, std::string> encode32_impl(std::span<const float> snapshots, std::span<const float> prev_snapshot,
                                          size_t num_snapshots, size_t snapshot_floats, ICompressor& compressor,
                                          OrderbookSimdCodecWorkspace& workspace) {
    const size_t num_floats = snapshots.size();
    float* f32_deltas_ptr = workspace.f32_deltas_.get();

    simd::XorFloat32_dispatcher(snapshots.data(), prev_snapshot.data(), f32_deltas_ptr, snapshot_floats);
    for (size_t s = 1; s < num_snapshots; ++s) {
        const float* current_snap = snapshots.data() + s * snapshot_floats;
        const float* prev_snap = snapshots.data() + (s - 1) * snapshot_floats;
        float* out_snap = f32_deltas_ptr + s * snapshot_floats;
        simd::XorFloat32_dispatcher(current_snap, prev_snap, out_snap, snapshot_floats);
    }

    const size_t f32_bytes = num_floats * sizeof(float);
    uint8_t* shuffled_bytes_ptr = workspace.shuffled_bytes().data();
    simd::ShuffleFloat32_dispatcher(f32_deltas_ptr, shuffled_bytes_ptr, num_floats);

    static_assert(sizeof(std::byte) == sizeof(uint8_t));
    std::span<const std::byte> data_to_compress(reinterpret_cast<const std::byte*>(shuffled_bytes_ptr), f32_bytes); // NOLINT
    return compressor.compress(data_to_compress);
}

}// namespace detail

class DynamicOrderbookSimdCodec {
public:
    explicit DynamicOrderbookSimdCodec(size_t depth, size_t features, std::unique_ptr<ICompressor> compressor)
        : depth_(depth), features_(features), snapshot_floats_(depth * features), compressor_(std::move(compressor)) {
        if (snapshot_floats_ == 0) {
            throw std::invalid_argument("Depth and features must be greater than zero.");
        }
        if (!compressor_) {
            throw std::invalid_argument("Compressor cannot be null.");
        }
    }

    DynamicOrderbookSimdCodec(DynamicOrderbookSimdCodec&&) noexcept = default;
    DynamicOrderbookSimdCodec& operator=(DynamicOrderbookSimdCodec&&) noexcept = default;

    DynamicOrderbookSimdCodec(const DynamicOrderbookSimdCodec&) = delete;
    DynamicOrderbookSimdCodec& operator=(const DynamicOrderbookSimdCodec&) = delete;

    std::expected<memory::vector<std::byte>, std::string> encode16(std::span<const float> snapshots, std::span<const float> prev_snapshot, OrderbookSimdCodecWorkspace& workspace) const;
    std::expected<std::vector<float>, std::string> decode16(std::span<const std::byte> encoded_data, size_t num_snapshots, std::span<float> prev_snapshot) const;

    std::expected<memory::vector<std::byte>, std::string> encode32(std::span<const float> snapshots, std::span<const float> prev_snapshot, OrderbookSimdCodecWorkspace& workspace) const;
    std::expected<std::vector<float>, std::string> decode32(std::span<const std::byte> encoded_data, size_t num_snapshots, std::span<float> prev_snapshot) const;

    [[nodiscard]] std::pair<size_t, size_t> get_depth_features_count() const
    {
        return std::make_pair(depth_, features_);
    }

    [[nodiscard]] size_t get_snapshot_size() const
    {
        return depth_ * features_;
    }


private:
    size_t depth_;
    size_t features_;
    size_t snapshot_floats_;
    std::unique_ptr<ICompressor> compressor_;
};

inline std::expected<memory::vector<std::byte>, std::string> DynamicOrderbookSimdCodec::encode16(std::span<const float> snapshots, std::span<const float> prev_snapshot, OrderbookSimdCodecWorkspace& workspace) const {
    if (prev_snapshot.size() != snapshot_floats_) {
        throw std::runtime_error("prev_snapshot size does not match configured snapshot_floats.");
    }
    const size_t num_floats = snapshots.size();
    if (num_floats == 0 || num_floats % snapshot_floats_ != 0) {
        throw std::runtime_error("Snapshot data size is not a multiple of the configured snapshot_floats.");
    }
    workspace.ensure_capacity(num_floats);
    return detail::encode16_impl(snapshots, prev_snapshot, num_floats / snapshot_floats_, snapshot_floats_, *compressor_, workspace);
}

inline std::expected<std::vector<float>, std::string> DynamicOrderbookSimdCodec::decode16(std::span<const std::byte> encoded_data, size_t num_snapshots, std::span<float> prev_snapshot) const {
    if (prev_snapshot.size() != snapshot_floats_) {
        return std::unexpected("prev_snapshot size does not match configured snapshot_floats.");
    }
    if (num_snapshots == 0) return std::vector<float>{};

    auto shuffled_f16_bytes_result = compressor_->decompress(encoded_data);
    if (!shuffled_f16_bytes_result) {
        return std::unexpected(shuffled_f16_bytes_result.error());
    }

    const size_t num_floats = num_snapshots * snapshot_floats_;
    const size_t f16_bytes = num_floats * sizeof(hwy::float16_t);
    if (shuffled_f16_bytes_result->size() != f16_bytes) {
        return std::unexpected("Decompressed data size does not match expected size for the given number of snapshots.");
    }

    std::vector<float> final_output(num_floats);
    static_assert(sizeof(std::byte) == sizeof(uint8_t));
    simd::UnshuffleAndReconstruct_dispatcher(reinterpret_cast<const uint8_t*>(shuffled_f16_bytes_result->data()), // NOLINT
                                       final_output.data(), num_snapshots, snapshot_floats_, prev_snapshot);

    return final_output;
}

inline std::expected<memory::vector<std::byte>, std::string> DynamicOrderbookSimdCodec::encode32(std::span<const float> snapshots, std::span<const float> prev_snapshot, OrderbookSimdCodecWorkspace& workspace) const {
    if (prev_snapshot.size() != snapshot_floats_) {
        throw std::runtime_error("prev_snapshot size does not match configured snapshot_floats.");
    }
    const size_t num_floats = snapshots.size();
    if (num_floats == 0 || num_floats % snapshot_floats_ != 0) {
        throw std::runtime_error("Snapshot data size is not a multiple of the configured snapshot_floats.");
    }
    workspace.ensure_capacity(num_floats);
    return detail::encode32_impl(snapshots, prev_snapshot, num_floats / snapshot_floats_, snapshot_floats_, *compressor_, workspace);
}

inline std::expected<std::vector<float>, std::string> DynamicOrderbookSimdCodec::decode32(std::span<const std::byte> encoded_data, size_t num_snapshots, std::span<float> prev_snapshot) const {
    if (prev_snapshot.size() != snapshot_floats_) {
        return std::unexpected("prev_snapshot size does not match configured snapshot_floats.");
    }
    if (num_snapshots == 0) return std::vector<float>{};

    auto shuffled_f32_bytes_result = compressor_->decompress(encoded_data);
    if (!shuffled_f32_bytes_result) {
        return std::unexpected(shuffled_f32_bytes_result.error());
    }

    const size_t num_floats = num_snapshots * snapshot_floats_;
    const size_t f32_bytes = num_floats * sizeof(float);
    if (shuffled_f32_bytes_result->size() != f32_bytes) {
        return std::unexpected("Decompressed data size does not match expected size for the given number of snapshots.");
    }

    std::vector<float> final_output(num_floats);
    static_assert(sizeof(std::byte) == sizeof(uint8_t));
    simd::UnshuffleAndReconstructFloat32_dispatcher(reinterpret_cast<const uint8_t*>(shuffled_f32_bytes_result->data()), // NOLINT
                                              final_output.data(), num_snapshots, snapshot_floats_, prev_snapshot);

    return final_output;
}

template <size_t Depth, size_t Features>
class OrderbookSimdCodec {
public:
    static constexpr size_t DepthSize = Depth;
    static constexpr size_t FeaturesSize = Features;
    static constexpr size_t SnapshotFloats = Depth * Features;
    using Snapshot = std::array<float, SnapshotFloats>;

    static_assert(SnapshotFloats > 0);

    explicit OrderbookSimdCodec(std::unique_ptr<ICompressor> compressor)
        : compressor_(std::move(compressor)) {
        if (!compressor_) {
            throw std::invalid_argument("Compressor cannot be null.");
        }
    }

    OrderbookSimdCodec(OrderbookSimdCodec&&) noexcept = default;
    OrderbookSimdCodec& operator=(OrderbookSimdCodec&&) noexcept = default;

    OrderbookSimdCodec(const OrderbookSimdCodec&) = delete;
    OrderbookSimdCodec& operator=(const OrderbookSimdCodec&) = delete;

    std::expected<memory::vector<std::byte>, std::string> encode16(std::span<const float> snapshots, const Snapshot& prev_snapshot, OrderbookSimdCodecWorkspace& workspace) const;
    std::expected<std::vector<float>, std::string> decode16(std::span<const std::byte> encoded_data, size_t num_snapshots, Snapshot& prev_snapshot) const;

    std::expected<memory::vector<std::byte>, std::string> encode32(std::span<const float> snapshots, const Snapshot& prev_snapshot, OrderbookSimdCodecWorkspace& workspace) const;
    std::expected<std::vector<float>, std::string> decode32(std::span<const std::byte> encoded_data, size_t num_snapshots, Snapshot& prev_snapshot) const;

private:
    std::unique_ptr<ICompressor> compressor_;
};

template <size_t Depth, size_t Features>
std::expected<memory::vector<std::byte>, std::string> OrderbookSimdCodec<Depth, Features>::encode16(std::span<const float> snapshots, const Snapshot& prev_snapshot, OrderbookSimdCodecWorkspace& workspace) const {
    const size_t num_floats = snapshots.size();
    if (num_floats == 0 || num_floats % SnapshotFloats != 0) {
        throw std::runtime_error("Snapshot data size is not a multiple of the configured SnapshotFloats.");
    }
    workspace.ensure_capacity(num_floats);
    return detail::encode16_impl(snapshots, {prev_snapshot.data(), SnapshotFloats}, num_floats / SnapshotFloats, SnapshotFloats, *compressor_, workspace);
}

template <size_t Depth, size_t Features>
std::expected<std::vector<float>, std::string> OrderbookSimdCodec<Depth, Features>::decode16(std::span<const std::byte> encoded_data, size_t num_snapshots, Snapshot& prev_snapshot) const {
    if (num_snapshots == 0) return std::vector<float>{};

    auto shuffled_f16_bytes_result = compressor_->decompress(encoded_data);
    if (!shuffled_f16_bytes_result) {
        return std::unexpected(shuffled_f16_bytes_result.error());
    }

    const size_t num_floats = num_snapshots * SnapshotFloats;
    const size_t f16_bytes = num_floats * sizeof(hwy::float16_t);
    if (shuffled_f16_bytes_result->size() != f16_bytes) {
        return std::unexpected("Decompressed data size does not match expected size for the given number of snapshots.");
    }

    std::vector<float> final_output(num_floats);
    static_assert(sizeof(std::byte) == sizeof(uint8_t));
    simd::UnshuffleAndReconstruct_dispatcher(reinterpret_cast<const uint8_t*>(shuffled_f16_bytes_result->data()), // NOLINT
                                       final_output.data(), num_snapshots, SnapshotFloats, {prev_snapshot.data(), SnapshotFloats});

    return final_output;
}

template <size_t Depth, size_t Features>
std::expected<memory::vector<std::byte>, std::string> OrderbookSimdCodec<Depth, Features>::encode32(std::span<const float> snapshots, const Snapshot& prev_snapshot, OrderbookSimdCodecWorkspace& workspace) const {
    const size_t num_floats = snapshots.size();
    if (num_floats == 0 || num_floats % SnapshotFloats != 0) {
        throw std::runtime_error("Snapshot data size is not a multiple of the configured SnapshotFloats.");
    }
    workspace.ensure_capacity(num_floats);
    return detail::encode32_impl(snapshots, {prev_snapshot.data(), SnapshotFloats}, num_floats / SnapshotFloats, SnapshotFloats, *compressor_, workspace);
}

template <size_t Depth, size_t Features>
std::expected<std::vector<float>, std::string> OrderbookSimdCodec<Depth, Features>::decode32(std::span<const std::byte> encoded_data, size_t num_snapshots, Snapshot& prev_snapshot) const {
    if (num_snapshots == 0) return std::vector<float>{};

    auto shuffled_f32_bytes_result = compressor_->decompress(encoded_data);
    if (!shuffled_f32_bytes_result) {
        return std::unexpected(shuffled_f32_bytes_result.error());
    }

    const size_t num_floats = num_snapshots * SnapshotFloats;
    const size_t f32_bytes = num_floats * sizeof(float);
    if (shuffled_f32_bytes_result->size() != f32_bytes) {
        return std::unexpected("Decompressed data size does not match expected size for the given number of snapshots.");
    }

    std::vector<float> final_output(num_floats);
    static_assert(sizeof(std::byte) == sizeof(uint8_t)); // NOLINT
    simd::UnshuffleAndReconstructFloat32_dispatcher(reinterpret_cast<const uint8_t*>(shuffled_f32_bytes_result->data()), // NOLINT
                                              final_output.data(), num_snapshots, SnapshotFloats, {prev_snapshot.data(), SnapshotFloats});

    return final_output;
}

using OkxObSimdCodec = OrderbookSimdCodec<codecs::Orderbook::OKX_DEPTH, codecs::Orderbook::OKX_FEATURES>;
using BitfinexObSimdCodec = OrderbookSimdCodec<codecs::Orderbook::OKX_DEPTH, codecs::Orderbook::OKX_FEATURES>; // Assuming same as OKX
using BinanceObSimdCodec = OrderbookSimdCodec<codecs::Orderbook::BINANCE_DEPTH, codecs::Orderbook::BINANCE_FEATURES>;

} // namespace cryptodd