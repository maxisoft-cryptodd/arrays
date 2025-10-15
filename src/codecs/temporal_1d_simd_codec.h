#pragma once

#include "i_compressor.h"
#include <expected>
#include <cstdint>
#include <hwy/aligned_allocator.h>
#include <memory>
#include <span>
#include <stdexcept>
#include "../memory/aligned.h"

namespace cryptodd {

using Float32AlignedVector = memory::AlignedVector<float, static_cast<std::size_t>(HWY_ALIGNMENT)>;
using Int64AlignedVector = memory::AlignedVector<int64_t, static_cast<std::size_t>(HWY_ALIGNMENT)>;
using ByteAlignedVector = memory::AlignedVector<std::byte, static_cast<std::size_t>(HWY_ALIGNMENT)>;
using ByteAlignedAllocator = ByteAlignedVector::allocator_type;

// Forward declarations for SIMD functions, now in their own namespace
namespace simd {
    void DemoteAndXor1D_dispatcher(const float* data, hwy::float16_t* out, size_t num_elements, float prev_element);
    void ShuffleFloat16_1D_dispatcher(const hwy::float16_t* in, uint8_t* out, size_t num_elements);
    void UnshuffleAndReconstruct16_1D_dispatcher(const uint8_t* shuffled_in, float* out, size_t num_elements, float& prev_element);

    void XorFloat32_1D_dispatcher(const float* data, float* out, size_t num_elements, float prev_element);
    void ShuffleFloat32_1D_dispatcher(const float* in, uint8_t* out, size_t num_elements);
    void UnshuffleAndReconstruct32_1D_dispatcher(const uint8_t* shuffled_in, float* out, size_t num_elements, float& prev_element);

    void XorInt64_1D_dispatcher(const int64_t* data, int64_t* out, size_t num_elements, int64_t prev_element);
    void UnXorInt64_1D_dispatcher(const int64_t* delta, int64_t* out, size_t num_elements, int64_t& prev_element);

    void DeltaInt64_1D_dispatcher(const int64_t* data, int64_t* out, size_t num_elements, int64_t prev_element);
    void CumulativeSumInt64_1D_dispatcher(const int64_t* delta, int64_t* out, size_t num_elements, int64_t& prev_element);
}

class Temporal1dSimdCodecWorkspace {
public:
    Temporal1dSimdCodecWorkspace() = default;
    Temporal1dSimdCodecWorkspace(const Temporal1dSimdCodecWorkspace&) = delete;
    Temporal1dSimdCodecWorkspace& operator=(const Temporal1dSimdCodecWorkspace&) = delete;
    Temporal1dSimdCodecWorkspace(Temporal1dSimdCodecWorkspace&&) noexcept = default;
    Temporal1dSimdCodecWorkspace& operator=(Temporal1dSimdCodecWorkspace&&) noexcept = default;

    void ensure_capacity(size_t required_elements) {
        if (capacity_in_elements_ >= required_elements) return;
        const size_t required_bytes = required_elements * sizeof(int64_t);
        buffer1_ = hwy::AllocateAligned<uint8_t>(required_bytes);
        buffer2_ = hwy::AllocateAligned<uint8_t>(required_bytes);
        if (!buffer1_ || !buffer2_) throw std::bad_alloc();
        capacity_in_elements_ = required_elements;
    }

    [[nodiscard]] hwy::AlignedFreeUniquePtr<uint8_t[]>& buffer1() { return buffer1_; }
    [[nodiscard]] hwy::AlignedFreeUniquePtr<uint8_t[]>& buffer2() { return buffer2_; }

private:
    hwy::AlignedFreeUniquePtr<uint8_t[]> buffer1_;
    hwy::AlignedFreeUniquePtr<uint8_t[]> buffer2_;
    size_t capacity_in_elements_ = 0;
};

class Temporal1dSimdCodec {
public:
    explicit Temporal1dSimdCodec(std::unique_ptr<ICompressor> compressor) : compressor_(std::move(compressor)) {
        if (!compressor_) throw std::invalid_argument("Compressor cannot be null.");
    }

    // Chain: float32 -> demote to float16 -> XOR -> shuffle
    std::expected<memory::vector<std::byte>, std::string> encode16_Xor_Shuffle(std::span<const float> data, float prev_element, Temporal1dSimdCodecWorkspace& workspace) const;
    std::expected<Float32AlignedVector, std::string> decode16_Xor_Shuffle(std::span<const std::byte> compressed, size_t num_elements, float& prev_element) const;

    // Chain: float32 -> XOR -> shuffle
    std::expected<memory::vector<std::byte>, std::string> encode32_Xor_Shuffle(std::span<const float> data, float prev_element, Temporal1dSimdCodecWorkspace& workspace) const;
    std::expected<Float32AlignedVector, std::string> decode32_Xor_Shuffle(std::span<const std::byte> compressed, size_t num_elements, float& prev_element) const;

    // Chain: int64 -> XOR
    std::expected<memory::vector<std::byte>, std::string> encode64_Xor(std::span<const int64_t> data, int64_t prev_element, Temporal1dSimdCodecWorkspace& workspace) const;
    std::expected<Int64AlignedVector, std::string> decode64_Xor(std::span<const std::byte> compressed, size_t num_elements, int64_t& prev_element) const;

    // Chain: int64 -> delta (subtraction)
    std::expected<memory::vector<std::byte>, std::string> encode64_Delta(std::span<const int64_t> data, int64_t prev_element, Temporal1dSimdCodecWorkspace& workspace) const;
    std::expected<Int64AlignedVector, std::string> decode64_Delta(std::span<const std::byte> compressed, size_t num_elements, int64_t& prev_element) const;

private:
    std::unique_ptr<ICompressor> compressor_;
};

// --- Implementation for Temporal1dSimdCodec ---

inline std::expected<memory::vector<std::byte>, std::string> Temporal1dSimdCodec::encode16_Xor_Shuffle(std::span<const float> data, float prev_element, Temporal1dSimdCodecWorkspace& workspace) const {
    workspace.ensure_capacity(data.size());
    auto* f16_deltas = reinterpret_cast<hwy::float16_t*>(workspace.buffer1().get());
    simd::DemoteAndXor1D_dispatcher(data.data(), f16_deltas, data.size(), prev_element);
    
    auto* shuffled_bytes = workspace.buffer2().get();
    simd::ShuffleFloat16_1D_dispatcher(f16_deltas, shuffled_bytes, data.size());

    static_assert(sizeof(std::byte) == sizeof(uint8_t));
    return compressor_->compress({reinterpret_cast<const std::byte*>(shuffled_bytes), data.size() * sizeof(hwy::float16_t)});
}

inline std::expected<Float32AlignedVector, std::string> Temporal1dSimdCodec::decode16_Xor_Shuffle(std::span<const std::byte> compressed, size_t num_elements, float& prev_element) const {
    auto shuffled_bytes_result = compressor_->decompress_to<ByteAlignedAllocator>(compressed);
    if (!shuffled_bytes_result) return std::unexpected(shuffled_bytes_result.error());
    if (shuffled_bytes_result->size() != num_elements * sizeof(hwy::float16_t)) return std::unexpected("Decompressed data size mismatch");

    Float32AlignedVector out_data(num_elements);
    static_assert(sizeof(std::byte) == sizeof(uint8_t));
    simd::UnshuffleAndReconstruct16_1D_dispatcher(reinterpret_cast<const uint8_t*>(shuffled_bytes_result->data()), out_data.data(), num_elements, prev_element);
    return out_data;
}

inline std::expected<memory::vector<std::byte>, std::string> Temporal1dSimdCodec::encode32_Xor_Shuffle(std::span<const float> data, float prev_element, Temporal1dSimdCodecWorkspace& workspace) const {
    workspace.ensure_capacity(data.size());
    auto* f32_deltas = reinterpret_cast<float*>(workspace.buffer1().get());
    simd::XorFloat32_1D_dispatcher(data.data(), f32_deltas, data.size(), prev_element);

    auto* shuffled_bytes = workspace.buffer2().get();
    simd::ShuffleFloat32_1D_dispatcher(f32_deltas, shuffled_bytes, data.size());

    static_assert(sizeof(std::byte) == sizeof(uint8_t));
    return compressor_->compress({reinterpret_cast<const std::byte*>(shuffled_bytes), data.size() * sizeof(float)});
}

inline std::expected<Float32AlignedVector, std::string> Temporal1dSimdCodec::decode32_Xor_Shuffle(std::span<const std::byte> compressed, size_t num_elements, float& prev_element) const {
    auto shuffled_bytes_result = compressor_->decompress_to<ByteAlignedAllocator>(compressed);
    if (!shuffled_bytes_result) return std::unexpected(shuffled_bytes_result.error());
    if (shuffled_bytes_result->size() != num_elements * sizeof(float)) return std::unexpected("Decompressed data size mismatch");

    Float32AlignedVector out_data(num_elements);
    static_assert(sizeof(std::byte) == sizeof(uint8_t));
    simd::UnshuffleAndReconstruct32_1D_dispatcher(reinterpret_cast<const uint8_t*>(shuffled_bytes_result->data()), out_data.data(), num_elements, prev_element);
    return out_data;
}

inline std::expected<memory::vector<std::byte>, std::string> Temporal1dSimdCodec::encode64_Xor(std::span<const int64_t> data, int64_t prev_element, Temporal1dSimdCodecWorkspace& workspace) const {
    workspace.ensure_capacity(data.size());
    auto* i64_deltas = reinterpret_cast<int64_t*>(workspace.buffer1().get());
    simd::XorInt64_1D_dispatcher(data.data(), i64_deltas, data.size(), prev_element);
    static_assert(sizeof(std::byte) == sizeof(uint8_t));
    return compressor_->compress({reinterpret_cast<const std::byte*>(i64_deltas), data.size() * sizeof(int64_t)});
}

inline std::expected<Int64AlignedVector, std::string> Temporal1dSimdCodec::decode64_Xor(std::span<const std::byte> compressed, size_t num_elements, int64_t& prev_element) const {
    auto delta_bytes_result = compressor_->decompress_to<ByteAlignedAllocator>(compressed);
    if (!delta_bytes_result) return std::unexpected(delta_bytes_result.error());
    if (delta_bytes_result->size() != num_elements * sizeof(int64_t)) return std::unexpected("Decompressed data size mismatch");

    Int64AlignedVector out_data(num_elements);
    simd::UnXorInt64_1D_dispatcher(reinterpret_cast<const int64_t*>(delta_bytes_result->data()), out_data.data(), num_elements, prev_element);
    return out_data;
}

inline std::expected<memory::vector<std::byte>, std::string> Temporal1dSimdCodec::encode64_Delta(std::span<const int64_t> data, int64_t prev_element, Temporal1dSimdCodecWorkspace& workspace) const {
    workspace.ensure_capacity(data.size());
    auto* i64_deltas = reinterpret_cast<int64_t*>(workspace.buffer1().get());
    simd::DeltaInt64_1D_dispatcher(data.data(), i64_deltas, data.size(), prev_element);
    static_assert(sizeof(std::byte) == sizeof(uint8_t));
    return compressor_->compress({reinterpret_cast<const std::byte*>(i64_deltas), data.size() * sizeof(int64_t)});
}

inline std::expected<Int64AlignedVector, std::string> Temporal1dSimdCodec::decode64_Delta(std::span<const std::byte> compressed, size_t num_elements, int64_t& prev_element) const {
    auto delta_bytes_result = compressor_->decompress_to<ByteAlignedAllocator>(compressed);
    if (!delta_bytes_result) return std::unexpected(delta_bytes_result.error());
    if (delta_bytes_result->size() != num_elements * sizeof(int64_t)) return std::unexpected("Decompressed data size mismatch");

    Int64AlignedVector out_data(num_elements);
    simd::CumulativeSumInt64_1D_dispatcher(reinterpret_cast<const int64_t*>(delta_bytes_result->data()), out_data.data(), num_elements, prev_element);
    return out_data;
}

} // namespace cryptodd
