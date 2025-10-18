#pragma once

#include "../data_io/data_reader.h"
#include "../data_io/data_writer.h"
#include "../data_io/data_compressor.h"
#include "../data_io/data_extractor.h" // ADDED
#include <nlohmann/json.hpp>
#include <expected>
#include <memory>
#include <map>
#include <mutex>
#include <span>
#include <optional>
#include <functional>

namespace cryptodd::ffi {

class ExpectedError
{
std::string m_message;
public:
    explicit ExpectedError(std::string message) : m_message(std::move(message))
    {
    }
    const std::string& message() {return m_message;}
};

class CddContext {
public:
    static std::expected<std::unique_ptr<CddContext>, ExpectedError> create(const nlohmann::json& config);

    std::expected<nlohmann::json, ExpectedError> execute_operation(
        const nlohmann::json& op_request,
        std::span<const std::byte> input_data,
        std::span<std::byte> output_data
    );

    // Getters for handlers
    std::optional<std::reference_wrapper<DataWriter>> get_writer();
    std::optional<std::reference_wrapper<DataReader>> get_reader();
    DataCompressor& get_compressor() { return compressor_; }
    DataExtractor& get_extractor() { return extractor_; } // ADDED
    
    // Method to get a zero-filled span for initial codec states, with caching.
    std::span<const std::byte> get_zero_state(size_t byte_size);

    CddContext(const CddContext&) = delete;
    CddContext& operator=(const CddContext&) = delete;
    CddContext(CddContext&&) = default;
    CddContext& operator=(CddContext&&) = default;
    ~CddContext();

private:

    std::unique_ptr<DataReader> reader_;
    std::unique_ptr<DataWriter> writer_;
    DataCompressor compressor_; // Reusable compressor
    DataExtractor extractor_; // ADDED

    // Cache for zero-initialized previous states to avoid repeated allocations.
    std::map<size_t, memory::vector<std::byte>> zero_state_cache_;
    std::mutex zero_state_cache_mutex_;

protected:
    struct ProtectedMarker{};

public:
    CddContext(ProtectedMarker, std::unique_ptr<DataReader>&& reader, std::unique_ptr<DataWriter>&& writer);
};

} // namespace cryptodd::ffi
