#pragma once

#include "i_compressor.h"
#include <hwy/base.h> // For hwy::float16_t
#include <array>
#include <hwy/aligned_allocator.h>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>
// Important: never ever include here ! #include <hwy/highway.h>
#ifdef HWY_HIGHWAY_INCLUDED
    static_assert(false, "highway.h is already included !");
#endif


namespace cryptodd {

void DemoteAndXor_dispatcher(const float* current, const float* prev, hwy::float16_t* out, size_t num_floats);
void ShuffleFloat16_dispatcher(const hwy::float16_t* in, uint8_t* out, size_t num_f16);
void UnshuffleAndReconstruct_dispatcher(const uint8_t* shuffled_in, float* out, size_t num_snapshots, size_t snapshot_floats, std::span<float> last_snapshot_state);

void XorFloat32_dispatcher(const float* current, const float* prev, float* out, size_t num_floats);
void ShuffleFloat32_dispatcher(const float* in, uint8_t* out, size_t num_f32);
void UnshuffleAndReconstructFloat32_dispatcher(const uint8_t* shuffled_in, float* out, size_t num_snapshots, size_t snapshot_floats, std::span<float> last_snapshot_state);

// A public struct to hold all reusable temporary buffers for encoding.
// The user of the codec is responsible for creating and managing this struct.
struct OrderbookSimdCodecWorkspace {
    hwy::AlignedFreeUniquePtr<hwy::float16_t[]> f16_deltas;
    hwy::AlignedFreeUniquePtr<float[]> f32_deltas;
    hwy::AlignedFreeUniquePtr<uint8_t[]> shuffled_bytes;
    size_t capacity_in_floats = 0;
};

// A helper function to ensure the workspace has enough capacity.
// This will only re-allocate if the required size is larger than the current capacity.
inline void EnsureCapacity(OrderbookSimdCodecWorkspace& workspace, size_t required_floats) {
    if (workspace.capacity_in_floats >= required_floats) return;

    workspace.f16_deltas = hwy::AllocateAligned<hwy::float16_t>(required_floats);
    workspace.f32_deltas = hwy::AllocateAligned<float>(required_floats);
    // Allocate enough for the largest possible shuffle (float32)
    workspace.shuffled_bytes = hwy::AllocateAligned<uint8_t>(required_floats * sizeof(float));

    workspace.capacity_in_floats = required_floats;
}

namespace detail {

inline std::vector<uint8_t> encode16_impl(std::span<const float> snapshots, std::span<const float> prev_snapshot,
                                          size_t num_snapshots, size_t snapshot_floats, ICompressor& compressor,
                                          OrderbookSimdCodecWorkspace& workspace) {
    const size_t num_floats = snapshots.size();

    DemoteAndXor_dispatcher(snapshots.data(), prev_snapshot.data(), workspace.f16_deltas.get(), snapshot_floats);
    for (size_t s = 1; s < num_snapshots; ++s) {
        const float* current_snap = snapshots.data() + s * snapshot_floats;
        const float* prev_snap = snapshots.data() + (s - 1) * snapshot_floats;
        hwy::float16_t* out_snap = workspace.f16_deltas.get() + s * snapshot_floats;
        DemoteAndXor_dispatcher(current_snap, prev_snap, out_snap, snapshot_floats);
    }

    ShuffleFloat16_dispatcher(workspace.f16_deltas.get(), workspace.shuffled_bytes.get(), num_floats);

    const size_t f16_bytes = num_floats * sizeof(hwy::float16_t);
    std::span<const uint8_t> data_to_compress(workspace.shuffled_bytes.get(), f16_bytes);

    return compressor.compress(data_to_compress);
}

inline std::vector<uint8_t> encode32_impl(std::span<const float> snapshots, std::span<const float> prev_snapshot,
                                          size_t num_snapshots, size_t snapshot_floats, ICompressor& compressor,
                                          OrderbookSimdCodecWorkspace& workspace) {
    const size_t num_floats = snapshots.size();

    XorFloat32_dispatcher(snapshots.data(), prev_snapshot.data(), workspace.f32_deltas.get(), snapshot_floats);
    for (size_t s = 1; s < num_snapshots; ++s) {
        const float* current_snap = snapshots.data() + s * snapshot_floats;
        const float* prev_snap = snapshots.data() + (s - 1) * snapshot_floats;
        float* out_snap = workspace.f32_deltas.get() + s * snapshot_floats;
        XorFloat32_dispatcher(current_snap, prev_snap, out_snap, snapshot_floats);
    }

    ShuffleFloat32_dispatcher(workspace.f32_deltas.get(), workspace.shuffled_bytes.get(), num_floats);

    const size_t f32_bytes = num_floats * sizeof(float);
    std::span<const uint8_t> data_to_compress(workspace.shuffled_bytes.get(), f32_bytes);

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

    std::vector<uint8_t> encode16(std::span<const float> snapshots, std::span<const float> prev_snapshot, OrderbookSimdCodecWorkspace& workspace) const;
    std::vector<float> decode16(std::span<const uint8_t> encoded_data, size_t num_snapshots, std::span<float> prev_snapshot) const;

    std::vector<uint8_t> encode32(std::span<const float> snapshots, std::span<const float> prev_snapshot, OrderbookSimdCodecWorkspace& workspace) const;
    std::vector<float> decode32(std::span<const uint8_t> encoded_data, size_t num_snapshots, std::span<float> prev_snapshot) const;

private:
    size_t depth_;
    size_t features_;
    size_t snapshot_floats_;
    std::unique_ptr<ICompressor> compressor_;
};

inline std::vector<uint8_t> DynamicOrderbookSimdCodec::encode16(std::span<const float> snapshots, std::span<const float> prev_snapshot, OrderbookSimdCodecWorkspace& workspace) const {
    if (prev_snapshot.size() != snapshot_floats_) {
        throw std::runtime_error("prev_snapshot size does not match configured snapshot_floats.");
    }
    const size_t num_floats = snapshots.size();
    if (num_floats == 0 || num_floats % snapshot_floats_ != 0) {
        throw std::runtime_error("Snapshot data size is not a multiple of the configured snapshot_floats.");
    }
    EnsureCapacity(workspace, num_floats);
    return detail::encode16_impl(snapshots, prev_snapshot, num_floats / snapshot_floats_, snapshot_floats_, *compressor_, workspace);
}

inline std::vector<float> DynamicOrderbookSimdCodec::decode16(std::span<const uint8_t> encoded_data, size_t num_snapshots, std::span<float> prev_snapshot) const {
    if (prev_snapshot.size() != snapshot_floats_) {
        throw std::runtime_error("prev_snapshot size does not match configured snapshot_floats.");
    }
    if (num_snapshots == 0) return {};

    std::vector<uint8_t> shuffled_f16_bytes = compressor_->decompress(encoded_data);

    const size_t num_floats = num_snapshots * snapshot_floats_;
    const size_t f16_bytes = num_floats * sizeof(hwy::float16_t);
    if (shuffled_f16_bytes.size() != f16_bytes) {
        throw std::runtime_error("Decompressed data size does not match expected size for the given number of snapshots.");
    }

    std::vector<float> final_output(num_floats);
    UnshuffleAndReconstruct_dispatcher(shuffled_f16_bytes.data(), final_output.data(), num_snapshots, snapshot_floats_, prev_snapshot);

    return final_output;
}

inline std::vector<uint8_t> DynamicOrderbookSimdCodec::encode32(std::span<const float> snapshots, std::span<const float> prev_snapshot, OrderbookSimdCodecWorkspace& workspace) const {
    if (prev_snapshot.size() != snapshot_floats_) {
        throw std::runtime_error("prev_snapshot size does not match configured snapshot_floats.");
    }
    const size_t num_floats = snapshots.size();
    if (num_floats == 0 || num_floats % snapshot_floats_ != 0) {
        throw std::runtime_error("Snapshot data size is not a multiple of the configured snapshot_floats.");
    }
    EnsureCapacity(workspace, num_floats);
    return detail::encode32_impl(snapshots, prev_snapshot, num_floats / snapshot_floats_, snapshot_floats_, *compressor_, workspace);
}

inline std::vector<float> DynamicOrderbookSimdCodec::decode32(std::span<const uint8_t> encoded_data, size_t num_snapshots, std::span<float> prev_snapshot) const {
    if (prev_snapshot.size() != snapshot_floats_) {
        throw std::runtime_error("prev_snapshot size does not match configured snapshot_floats.");
    }
    if (num_snapshots == 0) return {};

    std::vector<uint8_t> shuffled_f32_bytes = compressor_->decompress(encoded_data);

    const size_t num_floats = num_snapshots * snapshot_floats_;
    const size_t f32_bytes = num_floats * sizeof(float);
    if (shuffled_f32_bytes.size() != f32_bytes) {
        throw std::runtime_error("Decompressed data size does not match expected size for the given number of snapshots.");
    }

    std::vector<float> final_output(num_floats);
    UnshuffleAndReconstructFloat32_dispatcher(shuffled_f32_bytes.data(), final_output.data(), num_snapshots, snapshot_floats_, prev_snapshot);

    return final_output;
}

template <size_t Depth, size_t Features>
class OrderbookSimdCodec {
public:

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

    std::vector<uint8_t> encode16(std::span<const float> snapshots, const Snapshot& prev_snapshot, OrderbookSimdCodecWorkspace& workspace) const;
    std::vector<float> decode16(std::span<const uint8_t> encoded_data, size_t num_snapshots, Snapshot& prev_snapshot) const;

    std::vector<uint8_t> encode32(std::span<const float> snapshots, const Snapshot& prev_snapshot, OrderbookSimdCodecWorkspace& workspace) const;
    std::vector<float> decode32(std::span<const uint8_t> encoded_data, size_t num_snapshots, Snapshot& prev_snapshot) const;

private:
    std::unique_ptr<ICompressor> compressor_;
};

template <size_t Depth, size_t Features>
std::vector<uint8_t> OrderbookSimdCodec<Depth, Features>::encode16(std::span<const float> snapshots, const Snapshot& prev_snapshot, OrderbookSimdCodecWorkspace& workspace) const {
    const size_t num_floats = snapshots.size();
    if (num_floats == 0 || num_floats % SnapshotFloats != 0) {
        throw std::runtime_error("Snapshot data size is not a multiple of the configured SnapshotFloats.");
    }
    EnsureCapacity(workspace, num_floats);
    return detail::encode16_impl(snapshots, {prev_snapshot.data(), SnapshotFloats}, num_floats / SnapshotFloats, SnapshotFloats, *compressor_, workspace);
}

template <size_t Depth, size_t Features>
std::vector<float> OrderbookSimdCodec<Depth, Features>::decode16(std::span<const uint8_t> encoded_data, size_t num_snapshots, Snapshot& prev_snapshot) const {
    if (num_snapshots == 0) return {};

    std::vector<uint8_t> shuffled_f16_bytes = compressor_->decompress(encoded_data);

    const size_t num_floats = num_snapshots * SnapshotFloats;
    const size_t f16_bytes = num_floats * sizeof(hwy::float16_t);
    if (shuffled_f16_bytes.size() != f16_bytes) {
        throw std::runtime_error("Decompressed data size does not match expected size for the given number of snapshots.");
    }

    std::vector<float> final_output(num_floats);
    UnshuffleAndReconstruct_dispatcher(shuffled_f16_bytes.data(), final_output.data(), num_snapshots, SnapshotFloats, {prev_snapshot.data(), SnapshotFloats});

    return final_output;
}

template <size_t Depth, size_t Features>
std::vector<uint8_t> OrderbookSimdCodec<Depth, Features>::encode32(std::span<const float> snapshots, const Snapshot& prev_snapshot, OrderbookSimdCodecWorkspace& workspace) const {
    const size_t num_floats = snapshots.size();
    if (num_floats == 0 || num_floats % SnapshotFloats != 0) {
        throw std::runtime_error("Snapshot data size is not a multiple of the configured SnapshotFloats.");
    }
    EnsureCapacity(workspace, num_floats);
    return detail::encode32_impl(snapshots, {prev_snapshot.data(), SnapshotFloats}, num_floats / SnapshotFloats, SnapshotFloats, *compressor_, workspace);
}

template <size_t Depth, size_t Features>
std::vector<float> OrderbookSimdCodec<Depth, Features>::decode32(std::span<const uint8_t> encoded_data, size_t num_snapshots, Snapshot& prev_snapshot) const {
    if (num_snapshots == 0) return {};

    std::vector<uint8_t> shuffled_f32_bytes = compressor_->decompress(encoded_data);

    const size_t num_floats = num_snapshots * SnapshotFloats;
    const size_t f32_bytes = num_floats * sizeof(float);
    if (shuffled_f32_bytes.size() != f32_bytes) {
        throw std::runtime_error("Decompressed data size does not match expected size for the given number of snapshots.");
    }

    std::vector<float> final_output(num_floats);
    UnshuffleAndReconstructFloat32_dispatcher(shuffled_f32_bytes.data(), final_output.data(), num_snapshots, SnapshotFloats, {prev_snapshot.data(), SnapshotFloats});

    return final_output;
}

using OkxObSimdCodec = OrderbookSimdCodec<50, 3>;
using BitfinexObSimdCodec = OrderbookSimdCodec<50, 3>;
using BinanceObSimdCodec = OrderbookSimdCodec<256, 8>;

}