#pragma once

#include "../data_io/data_reader.h"
#include "../data_io/data_writer.h"
#include "../storage/i_storage_backend.h"
#include <nlohmann/json.hpp>
#include <expected>
#include <memory>
#include <span>

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
    // Factory function to create a context from JSON configuration
    static std::expected<std::unique_ptr<CddContext>, ExpectedError> create(const nlohmann::json& config);

    // Main execution entry point for this context
    std::expected<nlohmann::json, ExpectedError> execute_operation(
        const nlohmann::json& op_request,
        std::span<const std::byte> input_data,
        std::span<std::byte> output_data
    );

    // Explicitly non-copyable
    CddContext(const CddContext&) = delete;
    CddContext& operator=(const CddContext&) = delete;
    CddContext(CddContext&&) = default;
    CddContext& operator=(CddContext&&) = default;
    
    ~CddContext();

private:
    // Private constructor, use factory
    CddContext(std::unique_ptr<DataReader> reader, std::unique_ptr<DataWriter> writer);

    std::unique_ptr<DataReader> reader_;
    std::unique_ptr<DataWriter> writer_;
};

} // namespace cryptodd::ffi
