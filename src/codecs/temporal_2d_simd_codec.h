#pragma once

#include "i_compressor.h"
#include <array>
#include <cstdint>
#include <hwy/aligned_allocator.h>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

// Forward declarations for dispatcher functions. These are the entry points to the SIMD code.
namespace cryptodd {

namespace simd {
    void DemoteAndXor2D_dispatcher(const float* current, const float* prev, hwy::float16_t* out, size_t num_rows, size_t num_features);
    void ShuffleFloat16_2D_dispatcher(const hwy::float16_t* in, uint8_t* out, size_t num_rows, size_t num_features);
    void UnshuffleAndReconstruct16_2D_dispatcher(const uint8_t* shuffled_in, float* out, size_t num_rows, size_t num_features, std::span<float> prev_row_state);

    void XorFloat32_2D_dispatcher(const float* current, const float* prev, float* out, size_t num_rows, size_t num_features);
    void ShuffleFloat32_2D_dispatcher(const float* in, uint8_t* out, size_t num_rows, size_t num_features);
    void UnshuffleAndReconstruct32_2D_dispatcher(const uint8_t* shuffled_in, float* out, size_t num_rows, size_t num_features, std::span<float> prev_row_state);

    void XorInt64_2D_dispatcher(const int64_t* current, const int64_t* prev, int64_t* out, size_t num_rows, size_t num_features);
    void UnXorInt64_2D_dispatcher(const int64_t* delta, int64_t* out, size_t num_rows, size_t num_features, std::span<int64_t> prev_row_state);
}

class Temporal2dSimdCodecWorkspace;

namespace detail {
    // Implementation helpers to be shared between static and dynamic codecs.
    inline std::vector<uint8_t> encode16_2d_impl(std::span<const float> soa_data, std::span<const float> prev_row,
                                                size_t num_rows, size_t num_features, ICompressor& compressor,
                                                Temporal2dSimdCodecWorkspace& workspace);

    inline std::vector<uint8_t> encode32_2d_impl(std::span<const float> soa_data, std::span<const float> prev_row,
                                                size_t num_rows, size_t num_features, ICompressor& compressor,
                                                Temporal2dSimdCodecWorkspace& workspace);

    inline std::vector<uint8_t> encode64_2d_impl(std::span<const int64_t> soa_data, std::span<const int64_t> prev_row,
                                                size_t num_rows, size_t num_features, ICompressor& compressor,
                                                Temporal2dSimdCodecWorkspace& workspace);
}

class Temporal2dSimdCodecWorkspace {
public:
    Temporal2dSimdCodecWorkspace() = default;

    Temporal2dSimdCodecWorkspace(const Temporal2dSimdCodecWorkspace&) = delete;
    Temporal2dSimdCodecWorkspace& operator=(const Temporal2dSimdCodecWorkspace&) = delete;
    Temporal2dSimdCodecWorkspace(Temporal2dSimdCodecWorkspace&&) noexcept = default;
    Temporal2dSimdCodecWorkspace& operator=(Temporal2dSimdCodecWorkspace&&) noexcept = default;

    void ensure_capacity(const size_t required_elements) {
        if (capacity_in_elements_ >= required_elements) return;
        
        // Allocate enough space for the largest element size (int64/float)
        const size_t required_bytes = required_elements * sizeof(int64_t);
        buffer1_ = hwy::AllocateAligned<uint8_t>(required_bytes);
        buffer2_ = hwy::AllocateAligned<uint8_t>(required_bytes);

        if (!buffer1_ || !buffer2_) {
            throw std::bad_alloc();
        }
        capacity_in_elements_ = required_elements;
    }

    [[nodiscard]] hwy::AlignedFreeUniquePtr<uint8_t[]>& buffer1() { return buffer1_; }
    [[nodiscard]] hwy::AlignedFreeUniquePtr<uint8_t[]>& buffer2() { return buffer2_; }

private:
    friend std::vector<uint8_t> detail::encode16_2d_impl(std::span<const float>, std::span<const float>, size_t, size_t, ICompressor&, Temporal2dSimdCodecWorkspace&);
    friend std::vector<uint8_t> detail::encode32_2d_impl(std::span<const float>, std::span<const float>, size_t, size_t, ICompressor&, Temporal2dSimdCodecWorkspace&);
    friend std::vector<uint8_t> detail::encode64_2d_impl(std::span<const int64_t>, std::span<const int64_t>, size_t, size_t, ICompressor&, Temporal2dSimdCodecWorkspace&);

    hwy::AlignedFreeUniquePtr<uint8_t[]> buffer1_;
    hwy::AlignedFreeUniquePtr<uint8_t[]> buffer2_;
    size_t capacity_in_elements_ = 0;
};

namespace detail {

inline std::vector<uint8_t> encode16_2d_impl(std::span<const float> soa_data, std::span<const float> prev_row,
                                            size_t num_rows, size_t num_features, ICompressor& compressor,
                                            Temporal2dSimdCodecWorkspace& workspace) {
    const size_t total_elements = soa_data.size();
    auto* f16_deltas_ptr = reinterpret_cast<hwy::float16_t*>(workspace.buffer1().get());
    
    simd::DemoteAndXor2D_dispatcher(soa_data.data(), prev_row.data(), f16_deltas_ptr, num_rows, num_features);

    auto* shuffled_bytes_ptr = workspace.buffer2().get();
    simd::ShuffleFloat16_2D_dispatcher(f16_deltas_ptr, shuffled_bytes_ptr, num_rows, num_features);

    const size_t bytes_to_compress = total_elements * sizeof(hwy::float16_t);
    return compressor.compress({shuffled_bytes_ptr, bytes_to_compress});
}

inline std::vector<uint8_t> encode32_2d_impl(std::span<const float> soa_data, std::span<const float> prev_row,
                                            size_t num_rows, size_t num_features, ICompressor& compressor,
                                            Temporal2dSimdCodecWorkspace& workspace) {
    const size_t total_elements = soa_data.size();
    auto* f32_deltas_ptr = reinterpret_cast<float*>(workspace.buffer1().get());

    simd::XorFloat32_2D_dispatcher(soa_data.data(), prev_row.data(), f32_deltas_ptr, num_rows, num_features);

    auto* shuffled_bytes_ptr = workspace.buffer2().get();
    simd::ShuffleFloat32_2D_dispatcher(f32_deltas_ptr, shuffled_bytes_ptr, num_rows, num_features);

    const size_t bytes_to_compress = total_elements * sizeof(float);
    return compressor.compress({shuffled_bytes_ptr, bytes_to_compress});
}

inline std::vector<uint8_t> encode64_2d_impl(std::span<const int64_t> soa_data, std::span<const int64_t> prev_row,
                                            size_t num_rows, size_t num_features, ICompressor& compressor,
                                            Temporal2dSimdCodecWorkspace& workspace) {
    const size_t total_elements = soa_data.size();
    auto* i64_deltas_ptr = reinterpret_cast<int64_t*>(workspace.buffer1().get());

    simd::XorInt64_2D_dispatcher(soa_data.data(), prev_row.data(), i64_deltas_ptr, num_rows, num_features);

    const size_t bytes_to_compress = total_elements * sizeof(int64_t);
    return compressor.compress({reinterpret_cast<const uint8_t*>(i64_deltas_ptr), bytes_to_compress});
}

} // namespace detail

class DynamicTemporal2dSimdCodec {
public:
    explicit DynamicTemporal2dSimdCodec(size_t num_features, std::unique_ptr<ICompressor> compressor)
        : num_features_(num_features), compressor_(std::move(compressor)) {
        if (num_features_ == 0) throw std::invalid_argument("Features must be > 0.");
        if (!compressor_) throw std::invalid_argument("Compressor cannot be null.");
    }

    // Encode f32 -> demote to f16 -> XOR -> shuffle
    std::vector<uint8_t> encode16(std::span<const float> soa_data, std::span<const float> prev_row, Temporal2dSimdCodecWorkspace& workspace) const;
    std::vector<float> decode16(std::span<const uint8_t> compressed, std::span<float> prev_row) const;

    std::vector<uint8_t> encode32(std::span<const float> soa_data, std::span<const float> prev_row, Temporal2dSimdCodecWorkspace& workspace) const;
    std::vector<float> decode32(std::span<const uint8_t> compressed, std::span<float> prev_row) const;

    std::vector<uint8_t> encode64(std::span<const int64_t> soa_data, std::span<const int64_t> prev_row, Temporal2dSimdCodecWorkspace& workspace) const;
    std::vector<int64_t> decode64(std::span<const uint8_t> compressed, std::span<int64_t> prev_row) const;

private:
    size_t num_features_;
    std::unique_ptr<ICompressor> compressor_;
};


template <size_t NumFeatures>
class Temporal2dSimdCodec {
public:
    static constexpr size_t kNumFeatures = NumFeatures;
    using PrevRowFloat = std::array<float, kNumFeatures>;
    using PrevRowInt64 = std::array<int64_t, kNumFeatures>;

    static_assert(kNumFeatures > 0, "NumFeatures must be greater than zero.");

    explicit Temporal2dSimdCodec(std::unique_ptr<ICompressor> compressor) : compressor_(std::move(compressor)) {
        if (!compressor_) throw std::invalid_argument("Compressor cannot be null.");
    }

    // Encode f32 -> demote to f16 -> XOR -> shuffle
    std::vector<uint8_t> encode16(std::span<const float> soa_data, const PrevRowFloat& prev_row,
                                  Temporal2dSimdCodecWorkspace& workspace) const;
    std::vector<float> decode16(std::span<const uint8_t> compressed, size_t num_rows, PrevRowFloat& prev_row) const;

    // Encode f32 -> XOR -> shuffle
    std::vector<uint8_t> encode32(std::span<const float> soa_data, const PrevRowFloat& prev_row,
                                  Temporal2dSimdCodecWorkspace& workspace) const;
    std::vector<float> decode32(std::span<const uint8_t> compressed, size_t num_rows, PrevRowFloat& prev_row) const;

    // Encode i64 -> XOR
    std::vector<uint8_t> encode64(std::span<const int64_t> soa_data, const PrevRowInt64& prev_row,
                                  Temporal2dSimdCodecWorkspace& workspace) const;
    std::vector<int64_t> decode64(std::span<const uint8_t> compressed, size_t num_rows, PrevRowInt64& prev_row) const;

private:
    std::unique_ptr<ICompressor> compressor_;
};

// --- Implementation for DynamicTemporal2dSimdCodec ---

inline std::vector<uint8_t> DynamicTemporal2dSimdCodec::encode16(std::span<const float> soa_data, std::span<const float> prev_row, Temporal2dSimdCodecWorkspace& workspace) const {
    if (prev_row.size() != num_features_) throw std::runtime_error("Invalid prev_row size");
    if (soa_data.empty() || soa_data.size() % num_features_ != 0) throw std::runtime_error("Invalid soa_data size");
    const size_t num_rows = soa_data.size() / num_features_;
    workspace.ensure_capacity(soa_data.size());
    return detail::encode16_2d_impl(soa_data, prev_row, num_rows, num_features_, *compressor_, workspace);
}

inline std::vector<float> DynamicTemporal2dSimdCodec::decode16(std::span<const uint8_t> compressed, std::span<float> prev_row) const {
    if (prev_row.size() != num_features_) throw std::runtime_error("Invalid prev_row size");
    std::vector<uint8_t> shuffled_bytes = compressor_->decompress(compressed);
    if (shuffled_bytes.empty() || (shuffled_bytes.size() / sizeof(hwy::float16_t)) % num_features_ != 0) throw std::runtime_error("Decompressed data size mismatch");
    const size_t total_elements = shuffled_bytes.size() / sizeof(hwy::float16_t);
    const size_t num_rows = total_elements / num_features_;
    std::vector<float> out_data(total_elements);
    simd::UnshuffleAndReconstruct16_2D_dispatcher(shuffled_bytes.data(), out_data.data(), num_rows, num_features_, prev_row);
    return out_data;
}

inline std::vector<uint8_t> DynamicTemporal2dSimdCodec::encode32(std::span<const float> soa_data, std::span<const float> prev_row, Temporal2dSimdCodecWorkspace& workspace) const {
    if (prev_row.size() != num_features_) throw std::runtime_error("Invalid prev_row size");
    if (soa_data.empty() || soa_data.size() % num_features_ != 0) throw std::runtime_error("Invalid soa_data size");
    const size_t num_rows = soa_data.size() / num_features_;
    workspace.ensure_capacity(soa_data.size());
    return detail::encode32_2d_impl(soa_data, prev_row, num_rows, num_features_, *compressor_, workspace);
}

inline std::vector<float> DynamicTemporal2dSimdCodec::decode32(std::span<const uint8_t> compressed, std::span<float> prev_row) const {
    if (prev_row.size() != num_features_) throw std::runtime_error("Invalid prev_row size");
    std::vector<uint8_t> shuffled_bytes = compressor_->decompress(compressed);
    if (shuffled_bytes.empty() || (shuffled_bytes.size() / sizeof(float)) % num_features_ != 0) throw std::runtime_error("Decompressed data size mismatch");
    const size_t total_elements = shuffled_bytes.size() / sizeof(float);
    const size_t num_rows = total_elements / num_features_;
    std::vector<float> out_data(total_elements);
    simd::UnshuffleAndReconstruct32_2D_dispatcher(shuffled_bytes.data(), out_data.data(), num_rows, num_features_, prev_row);
    return out_data;
}

inline std::vector<uint8_t> DynamicTemporal2dSimdCodec::encode64(std::span<const int64_t> soa_data, std::span<const int64_t> prev_row, Temporal2dSimdCodecWorkspace& workspace) const {
    if (prev_row.size() != num_features_) throw std::runtime_error("Invalid prev_row size");
    if (soa_data.empty() || soa_data.size() % num_features_ != 0) throw std::runtime_error("Invalid soa_data size");
    const size_t num_rows = soa_data.size() / num_features_;
    workspace.ensure_capacity(soa_data.size());
    return detail::encode64_2d_impl(soa_data, prev_row, num_rows, num_features_, *compressor_, workspace);
}

inline std::vector<int64_t> DynamicTemporal2dSimdCodec::decode64(std::span<const uint8_t> compressed, std::span<int64_t> prev_row) const {
    if (prev_row.size() != num_features_) throw std::runtime_error("Invalid prev_row size");
    std::vector<uint8_t> delta_bytes = compressor_->decompress(compressed);
    if (delta_bytes.empty() || (delta_bytes.size() / sizeof(int64_t)) % num_features_ != 0) throw std::runtime_error("Decompressed data size mismatch");
    const size_t total_elements = delta_bytes.size() / sizeof(int64_t);
    const size_t num_rows = total_elements / num_features_;
    std::vector<int64_t> out_data(total_elements);
    simd::UnXorInt64_2D_dispatcher(reinterpret_cast<const int64_t*>(delta_bytes.data()), out_data.data(), num_rows, num_features_, prev_row);
    return out_data;
}

// --- Implementation for Temporal2dSimdCodec (static) ---

template <size_t NF>
std::vector<uint8_t> Temporal2dSimdCodec<NF>::encode16(std::span<const float> soa_data, const PrevRowFloat& prev_row, Temporal2dSimdCodecWorkspace& workspace) const {
    if (soa_data.empty() || soa_data.size() % kNumFeatures != 0) throw std::runtime_error("Invalid soa_data size");
    const size_t num_rows = soa_data.size() / kNumFeatures;
    workspace.ensure_capacity(soa_data.size());
    return detail::encode16_2d_impl(soa_data, {prev_row.data(), NF}, num_rows, NF, *compressor_, workspace);
}

template <size_t NF>
std::vector<float> Temporal2dSimdCodec<NF>::decode16(std::span<const uint8_t> compressed, size_t num_rows, PrevRowFloat& prev_row) const {
    const size_t total_elements = num_rows * kNumFeatures;
    std::vector<uint8_t> shuffled_bytes = compressor_->decompress(compressed);
    if (shuffled_bytes.size() != total_elements * sizeof(hwy::float16_t)) throw std::runtime_error("Decompressed data size mismatch");
    std::vector<float> out_data(total_elements);
    simd::UnshuffleAndReconstruct16_2D_dispatcher(shuffled_bytes.data(), out_data.data(), num_rows, NF, {prev_row.data(), NF});
    return out_data;
}

template <size_t NF>
std::vector<uint8_t> Temporal2dSimdCodec<NF>::encode32(std::span<const float> soa_data, const PrevRowFloat& prev_row, Temporal2dSimdCodecWorkspace& workspace) const {
    if (soa_data.empty() || soa_data.size() % kNumFeatures != 0) throw std::runtime_error("Invalid soa_data size");
    const size_t num_rows = soa_data.size() / kNumFeatures;
    workspace.ensure_capacity(soa_data.size());
    return detail::encode32_2d_impl(soa_data, {prev_row.data(), NF}, num_rows, NF, *compressor_, workspace);
}

template <size_t NF>
std::vector<float> Temporal2dSimdCodec<NF>::decode32(std::span<const uint8_t> compressed, size_t num_rows, PrevRowFloat& prev_row) const {
    const size_t total_elements = num_rows * kNumFeatures;
    std::vector<uint8_t> shuffled_bytes = compressor_->decompress(compressed);
    if (shuffled_bytes.size() != total_elements * sizeof(float)) throw std::runtime_error("Decompressed data size mismatch");
    std::vector<float> out_data(total_elements);
    simd::UnshuffleAndReconstruct32_2D_dispatcher(shuffled_bytes.data(), out_data.data(), num_rows, NF, {prev_row.data(), NF});
    return out_data;
}

template <size_t NF>
std::vector<uint8_t> Temporal2dSimdCodec<NF>::encode64(std::span<const int64_t> soa_data, const PrevRowInt64& prev_row, Temporal2dSimdCodecWorkspace& workspace) const {
    if (soa_data.empty() || soa_data.size() % kNumFeatures != 0) throw std::runtime_error("Invalid soa_data size");
    const size_t num_rows = soa_data.size() / kNumFeatures;
    workspace.ensure_capacity(soa_data.size());
    return detail::encode64_2d_impl(soa_data, {prev_row.data(), NF}, num_rows, NF, *compressor_, workspace);
}

template <size_t NF>
std::vector<int64_t> Temporal2dSimdCodec<NF>::decode64(std::span<const uint8_t> compressed, size_t num_rows, PrevRowInt64& prev_row) const {
    const size_t total_elements = num_rows * kNumFeatures;
    std::vector<uint8_t> delta_bytes = compressor_->decompress(compressed);
    if (delta_bytes.size() != total_elements * sizeof(int64_t)) throw std::runtime_error("Decompressed data size mismatch");
    std::vector<int64_t> out_data(total_elements);
    simd::UnXorInt64_2D_dispatcher(reinterpret_cast<const int64_t*>(delta_bytes.data()), out_data.data(), num_rows, NF, {prev_row.data(), NF});
    return out_data;
}

} // namespace cryptodd