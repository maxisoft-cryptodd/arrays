#include "buffer.h"
#include "cdd_file_format.h" // For DType enum

#include <gtest/gtest.h>
#include <hwy/aligned_allocator.h> // For hwy::kAlignment
#include <numeric>

using namespace cryptodd;

// This is a critical sanity check to ensure our custom aligned vector's
// alignment constant matches the one defined and used by the Highway library.
// A mismatch could lead to incorrect assumptions and alignment-related crashes.
TEST(BufferSanityTest, HWYAlignmentConstantMatches)
{
    EXPECT_EQ(cryptodd::details::_DEFAULT_HWY_ALIGNMENT, HWY_ALIGNMENT) << "The local _DEFAULT_HWY_ALIGNMENT constant must match the one from the Highway library.";
}

TEST(BufferTest, DefaultConstruction)
{
    Buffer b;
    EXPECT_EQ(b.element_count(), 0);
    EXPECT_EQ(b.byte_size(), 0);
    EXPECT_EQ(b.dtype(), DType::UINT8); // Default is vector<std::byte>, which maps to UINT8
    EXPECT_TRUE(b.get<std::byte>().empty());
    EXPECT_TRUE(b.as_bytes().empty());
}

// Define a struct to hold parameters for our typed tests
struct BufferTypeParam
{
    std::string type_name;
    DType expected_dtype;
    std::function<Buffer()> buffer_factory;
    std::function<void(Buffer &)> verifier;
};

// A test fixture for parameterized tests on different buffer types
class BufferTypeTest : public ::testing::TestWithParam<BufferTypeParam>
{
};

TEST_P(BufferTypeTest, ConstructionAndAccess)
{
    auto param = GetParam();
    Buffer b = param.buffer_factory();

    EXPECT_EQ(b.element_count(), 100);
    EXPECT_EQ(b.dtype(), param.expected_dtype);
    param.verifier(b);
}

// Instantiate the test suite with parameters for each vector type in the variant
INSTANTIATE_TEST_SUITE_P(
    AllBufferTypes, BufferTypeTest,
    ::testing::Values(
        BufferTypeParam{"StdVector_uint8_t", DType::UINT8,
                        []() {
                            auto vec = memory::vector<uint8_t>(100);
                            std::iota(vec.begin(), vec.end(), 0);
                            return Buffer(std::move(vec));
                        },
                        [](Buffer &b) {
                            EXPECT_EQ(b.byte_size(), 100 * sizeof(uint8_t));
                            auto span = b.get<uint8_t>();
                            ASSERT_EQ(span.size(), 100);
                            EXPECT_EQ(span[99], 99);
                        }},
        BufferTypeParam{"StdVector_float", DType::FLOAT32,
                        []() {
                            auto vec = memory::vector<float>(100);
                            std::iota(vec.begin(), vec.end(), 0.5f);
                            return Buffer(std::move(vec));
                        },
                        [](Buffer &b) {
                            EXPECT_EQ(b.byte_size(), 100 * sizeof(float));
                            auto span = b.get<float>();
                            ASSERT_EQ(span.size(), 100);
                            EXPECT_FLOAT_EQ(span[50], 50.5f);
                        }},
        BufferTypeParam{"StdVector_int64_t", DType::INT64,
                        []() {
                            auto vec = memory::vector<int64_t>(100);
                            std::iota(vec.begin(), vec.end(), 1);
                            return Buffer(std::move(vec));
                        },
                        [](Buffer &b) {
                            EXPECT_EQ(b.byte_size(), 100 * sizeof(int64_t));
                            auto span = b.get<int64_t>();
                            ASSERT_EQ(span.size(), 100);
                            EXPECT_EQ(span[99], 100);
                        }},
        BufferTypeParam{"StdVector_std_byte", DType::UINT8,
                        []() {
                            auto vec = memory::vector<std::byte>(100);
                            for (size_t i = 0; i < 100; ++i)
                                vec[i] = std::byte(i);
                            return Buffer(std::move(vec));
                        },
                        [](Buffer &b) {
                            EXPECT_EQ(b.byte_size(), 100 * sizeof(std::byte));
                            auto span = b.get<std::byte>();
                            ASSERT_EQ(span.size(), 100);
                            EXPECT_EQ(span[99], std::byte(99));
                        }},
        BufferTypeParam{"AlignedVector_float", DType::FLOAT32,
                        []() {
                            auto vec = details::Float32AlignedVector(100);
                            std::iota(vec.begin(), vec.end(), 0.5f);
                            return Buffer(std::move(vec));
                        },
                        [](Buffer &b) {
                            EXPECT_EQ(b.byte_size(), 100 * sizeof(float));
                            auto span = b.get<float>();
                            ASSERT_EQ(span.size(), 100);
                            EXPECT_FLOAT_EQ(span[50], 50.5f);
                        }},
        BufferTypeParam{"AlignedVector_int64_t", DType::INT64,
                        []() {
                            auto vec = details::Int64AlignedVector(100);
                            std::iota(vec.begin(), vec.end(), 1);
                            return Buffer(std::move(vec));
                        },
                        [](Buffer &b) {
                            EXPECT_EQ(b.byte_size(), 100 * sizeof(int64_t));
                            auto span = b.get<int64_t>();
                            ASSERT_EQ(span.size(), 100);
                            EXPECT_EQ(span[99], 100);
                        }}),
    [](const auto &info) {
        return info.param.type_name;
    });

TEST(BufferTest, GetAsDifferentType)
{
    auto vec = memory::vector<float>(10);
    std::iota(vec.begin(), vec.end(), 1.0f);
    Buffer b(std::move(vec));

    // Get as bytes
    auto byte_span = b.as_bytes();
    EXPECT_EQ(byte_span.size(), 10 * sizeof(float));

    // Get as uint32_t (same size)
    auto uint32_span = b.get<uint32_t>();
    EXPECT_EQ(uint32_span.size(), 10);

    // Get as uint8_t (4x elements)
    auto uint8_span = b.get<uint8_t>();
    EXPECT_EQ(uint8_span.size(), 10 * 4);

    // Get as int64_t (half the elements)
    auto int64_span = b.get<int64_t>();
    EXPECT_EQ(int64_span.size(), 10 / 2);
}

TEST(BufferTest, GetAsIncompatibleTypeThrows)
{
    auto vec = memory::vector<uint8_t>(7); // 7 bytes is not divisible by 4
    Buffer b(std::move(vec));

    EXPECT_THROW({ b.get<float>(); }, std::runtime_error);
    EXPECT_THROW({ b.get<uint32_t>(); }, std::runtime_error);
    EXPECT_THROW({ b.get<int16_t>(); }, std::runtime_error);
}

TEST(BufferTest, MoveSemantics)
{
    auto vec = memory::vector<float>(50);
    std::iota(vec.begin(), vec.end(), 1.0f);
    Buffer b1(std::move(vec));

    const size_t initial_count = b1.element_count();
    const size_t initial_bytes = b1.byte_size();
    const DType initial_dtype = b1.dtype();

    ASSERT_EQ(initial_count, 50);

    // Test move constructor
    Buffer b2 = std::move(b1);
    EXPECT_EQ(b2.element_count(), initial_count);
    EXPECT_EQ(b2.byte_size(), initial_bytes);
    EXPECT_EQ(b2.dtype(), initial_dtype);
    EXPECT_EQ(b1.element_count(), 0); // b1 should be empty

    // Test move assignment
    Buffer b3;
    ASSERT_EQ(b3.element_count(), 0);
    b3 = std::move(b2);

    EXPECT_EQ(b3.element_count(), initial_count);
    EXPECT_EQ(b3.byte_size(), initial_bytes);
    EXPECT_EQ(b3.dtype(), initial_dtype);
    EXPECT_EQ(b2.element_count(), 0); // b2 should be empty
}