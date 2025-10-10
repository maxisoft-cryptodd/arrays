#pragma once

/* FOR TESTING choose one of the following options:
 *
 * #define HWY_COMPILE_ONLY_STATIC 1
 * #define HWY_COMPILE_ONLY_SCALAR 1
 * #define HWY_COMPILE_ONLY_EMU128 1
*/

#include <span>
#include <array>
#include <stdexcept>
#include <vector>
#include <memory>
#include "i_compressor.h"

// These are needed for the implementation in this header
#include "hwy/aligned_allocator.h"

namespace cryptodd {

// Forward declarations for the SIMD dispatcher functions defined in the .cpp file.
// These functions bridge the gap between the templated class and the non-templated,
// dynamically-dispatched SIMD code.
void DemoteAndXor_dispatcher(const float* current, const float* prev, hwy::float16_t* out, size_t num_floats);
void ShuffleFloat16_dispatcher(const hwy::float16_t* in, uint8_t* out, size_t num_f16);
void UnshuffleAndReconstruct_dispatcher(const uint8_t* shuffled_in, float* out, size_t num_snapshots, size_t snapshot_floats, std::span<float> last_snapshot_state);

template <size_t Depth, size_t Features>
class OkxObSimdCodec {
public:
    // The total number of float values in a single snapshot.
    static constexpr size_t SnapshotFloats = Depth * Features;
    using OkxSnapshot = std::array<float, SnapshotFloats>;

    static_assert(SnapshotFloats > 0);

    /**
     * @brief Constructs the codec using a specific compressor implementation.
     * @param compressor A smart pointer to an object that implements the ICompressor interface.
     */
    explicit OkxObSimdCodec(std::unique_ptr<ICompressor> compressor)
        : compressor_(std::move(compressor)) {
        if (!compressor_) {
            throw std::invalid_argument("Compressor cannot be null.");
        }
    }

    // Default move semantics are fine now that we just own a unique_ptr.
    OkxObSimdCodec(OkxObSimdCodec&&) noexcept = default;
    OkxObSimdCodec& operator=(OkxObSimdCodec&&) noexcept = default;

    // Copying is deleted because the compressor is unique.
    OkxObSimdCodec(const OkxObSimdCodec&) = delete;
    OkxObSimdCodec& operator=(const OkxObSimdCodec&) = delete;

    std::vector<uint8_t> encode(std::span<const float> snapshots, const OkxSnapshot& prev_snapshot);
    std::vector<float> decode(std::span<const uint8_t> encoded_data, size_t num_snapshots, OkxSnapshot& prev_snapshot);

private:
    std::unique_ptr<ICompressor> compressor_;
};

// =================================================================================
// IMPLEMENTATION of templated methods
// Must be in the header file.
// =================================================================================

template <size_t Depth, size_t Features>
std::vector<uint8_t> OkxObSimdCodec<Depth, Features>::encode(std::span<const float> snapshots, const OkxSnapshot& prev_snapshot) {
    const size_t num_floats = snapshots.size();
    if (num_floats == 0 || num_floats % SnapshotFloats != 0) {
        throw std::runtime_error("Snapshot data size is not a multiple of the configured SnapshotFloats.");
    }
    const size_t num_snapshots = num_floats / SnapshotFloats;

    auto f16_deltas_buffer = hwy::AllocateAligned<hwy::float16_t>(num_floats);
    auto shuffled_buffer = hwy::AllocateAligned<uint8_t>(num_floats * sizeof(hwy::float16_t)); // NOLINT

    DemoteAndXor_dispatcher(snapshots.data(), prev_snapshot.data(), f16_deltas_buffer.get(), SnapshotFloats);
    for (size_t s = 1; s < num_snapshots; ++s) {
        const float* current_snap = snapshots.data() + s * SnapshotFloats;
        const float* prev_snap = snapshots.data() + (s - 1) * SnapshotFloats;
        hwy::float16_t* out_snap = f16_deltas_buffer.get() + s * SnapshotFloats;
        DemoteAndXor_dispatcher(current_snap, prev_snap, out_snap, SnapshotFloats);
    }

    ShuffleFloat16_dispatcher(f16_deltas_buffer.get(), shuffled_buffer.get(), num_floats);

    const size_t f16_bytes = num_floats * sizeof(hwy::float16_t);
    std::span<const uint8_t> data_to_compress(shuffled_buffer.get(), f16_bytes);

    // DELEGATE compression to the polymorphic object.
    return compressor_->compress(data_to_compress);
}

template <size_t Depth, size_t Features>
std::vector<float> OkxObSimdCodec<Depth, Features>::decode(std::span<const uint8_t> encoded_data, size_t num_snapshots, OkxSnapshot& prev_snapshot) {
    if (num_snapshots == 0) return {};

    // DELEGATE decompression to the polymorphic object.
    std::vector<uint8_t> shuffled_f16_bytes = compressor_->decompress(encoded_data);

    // Perform sanity checks on the decompressed size.
    const size_t num_floats = num_snapshots * SnapshotFloats;
    const size_t f16_bytes = num_floats * sizeof(hwy::float16_t);
    if (shuffled_f16_bytes.size() != f16_bytes) {
        throw std::runtime_error("Decompressed data size does not match expected size for the given number of snapshots.");
    }

    // Your SIMD UnshuffleAndReconstruct logic remains identical.
    std::vector<float> final_output(num_floats);
    UnshuffleAndReconstruct_dispatcher(shuffled_f16_bytes.data(), final_output.data(), num_snapshots, SnapshotFloats, prev_snapshot);

    return final_output;
}

using OkxObSimdCodecDefault = OkxObSimdCodec<50, 3>;

} // namespace cryptodd
