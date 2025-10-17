#pragma once

#include <cstddef>
#include <span>
#include <stdexcept>
#include <variant>
#include <vector>

#include "../file_format/cdd_file_format.h"
#include "../memory/aligned.h"

namespace cryptodd
{

    namespace details
    {
        // ReSharper disable once CppRedundantCastExpression
        inline constexpr size_t _DEFAULT_HWY_ALIGNMENT = static_cast<size_t>( // NOLINT(*-reserved-identifier)
#ifdef USER_HWY_ALIGNMENT
            USER_HWY_ALIGNMENT
#elifdef HWY_ALIGNMENT
            HWY_ALIGNMENT
#else
            128
#endif
        ); // expected, need to set to current highway value

        static_assert(_DEFAULT_HWY_ALIGNMENT > 0);

        using Float32AlignedVector = memory::AlignedVector<float, _DEFAULT_HWY_ALIGNMENT>;
        using ByteAlignedVector = memory::AlignedVector<std::byte, _DEFAULT_HWY_ALIGNMENT>;
        using Int64AlignedVector = memory::AlignedVector<int64_t, _DEFAULT_HWY_ALIGNMENT>;
    } // namespace details

    using vect_variant =
        std::variant<memory::vector<uint8_t>, memory::vector<float>, memory::vector<int64_t>, memory::vector<std::byte>,
                     details::Float32AlignedVector, details::ByteAlignedVector, details::Int64AlignedVector>;

    class Buffer
    {
        vect_variant m_data;

    public:
        Buffer() : m_data(memory::vector<std::byte>())
        {
        }

        explicit Buffer(memory::vector<uint8_t>&& data) : m_data(std::move(data))
        {
        }

        explicit Buffer(memory::vector<float>&& data) : m_data(std::move(data))
        {
        }

        explicit Buffer(memory::vector<int64_t>&& data) : m_data(std::move(data))
        {
        }

        explicit Buffer(memory::vector<std::byte>&& data) : m_data(std::move(data))
        {
        }

        explicit Buffer(details::Float32AlignedVector&& data) : m_data(std::move(data))
        {
        }

        explicit Buffer(details::ByteAlignedVector&& data) : m_data(std::move(data))
        {
        }

        explicit Buffer(details::Int64AlignedVector&& data) : m_data(std::move(data))
        {
        }

        Buffer(const Buffer& other) = delete;
        Buffer& operator=(const Buffer& other) = delete;

        Buffer(Buffer&& other) noexcept : m_data(std::move(other.m_data))
        {
        }

        Buffer& operator=(Buffer&& other) noexcept
        {
            if (this != &other)
            {
                m_data = std::move(other.m_data);
            }
            return *this;
        }

        template <typename T>
        std::span<T> get()
        {
            return std::visit(
                []<typename T0>(T0& vec) -> std::span<T>
                {
                    using SourceType = std::decay_t<T0>::value_type;
                    // If the source and target types are the same, we can return a span directly.
                    if constexpr (std::is_same_v<SourceType, T>)
                    {
                        return vec;
                    }
                    else
                    {
                        // Otherwise, perform a byte-level reinterpret_cast.
                        const size_t byte_size = vec.size() * sizeof(SourceType);
                        if (byte_size % sizeof(T) != 0)
                        {
                            throw std::runtime_error("Buffer data size is not evenly divisible by target type size.");
                        }
                        return {reinterpret_cast<T*>(vec.data()), byte_size / sizeof(T)};
                    }
                },
                m_data);
        }

        std::span<std::byte> as_bytes()
        {
            return get<std::byte>();
        }

        [[nodiscard]] size_t element_count() const
        {
            return std::visit([](const auto& vec) { return vec.size(); }, m_data);
        }

        [[nodiscard]] size_t byte_size() const
        {
            return std::visit(
                []<typename T0>(const T0& vec)
                {
                    using V = std::decay_t<T0>::value_type;
                    return vec.size() * sizeof(V);
                },
                m_data);
        }

        [[nodiscard]] DType dtype() const
        {
            return std::visit(
                []<typename T0>(const T0& vec) -> DType
                {
                    using T = std::decay_t<T0>::value_type;
                    if constexpr (std::is_same_v<T, double>)
                    {
                        return DType::FLOAT64;
                    }
                    else if constexpr (std::is_same_v<T, float>)
                    {
                        return DType::FLOAT32;
                    }
                    else if constexpr (std::is_same_v<T, uint8_t>)
                    { // NOLINT(*-branch-clone)

                        return DType::UINT8;
                    }
                    else if constexpr (std::is_same_v<T, int64_t>)
                    {
                        return DType::INT64;
                    }
                    else if constexpr (std::is_same_v<T, std::byte>)
                    {
                        return DType::UINT8;
                    }

                    std::unreachable();
                },
                m_data);
        }
    };

} // namespace cryptodd
