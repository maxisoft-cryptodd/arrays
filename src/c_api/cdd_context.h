#pragma once

#include <expected>
#include <memory>
#include <string>
#include <optional>
#include <functional>
#include <span>
#include <map>
#include <mutex>
#include <atomic>
#include <nlohmann/json_fwd.hpp>

#include "../data_io/data_reader.h"
#include "../data_io/data_writer.h"
#include "../data_io/data_compressor.h"
#include "../data_io/data_extractor.h"

namespace cryptodd::ffi {

class ExpectedError
{
    std::string m_message;
public:
    explicit ExpectedError(std::string message) : m_message(std::move(message)) {}
    const std::string& message() const { return m_message; }
};

// NEW: RAII guard for detecting concurrent access
class [[nodiscard]] ConcurrencyGuard {
    std::atomic<bool>& in_use_flag_;
    bool is_locked_{false};
public:
    explicit ConcurrencyGuard(std::atomic<bool>& flag) : in_use_flag_(flag) {
        bool expected = false;
        // Attempt to atomically set the flag from false to true.
        // If 'expected' is still true after this call, it means the flag was already true.
        is_locked_ = in_use_flag_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }

    ~ConcurrencyGuard() {
        if (is_locked_) {
            // If we successfully acquired the lock, release it.
            in_use_flag_.store(false, std::memory_order_release);
        }
    }
    
    // Allow checking if the lock was acquired
    operator bool() const {
        return is_locked_;
    }
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
    std::optional<std::reference_wrapper<cryptodd::DataWriter>> get_writer();
    std::optional<std::reference_wrapper<cryptodd::DataReader>> get_reader();
    cryptodd::DataCompressor& get_compressor() { return compressor_; }
    cryptodd::DataExtractor& get_extractor() { return extractor_; }
    std::span<const std::byte> get_zero_state(size_t byte_size);
    CddContext(const CddContext&) = delete;
    CddContext& operator=(const CddContext&) = delete;
    CddContext(CddContext&&) = default;
    CddContext& operator=(CddContext&&) = default;
    ~CddContext();

private:
    std::unique_ptr<cryptodd::DataReader> reader_;
    std::unique_ptr<cryptodd::DataWriter> writer_;
    cryptodd::DataCompressor compressor_;
    cryptodd::DataExtractor extractor_;
    std::map<size_t, cryptodd::memory::vector<std::byte>> zero_state_cache_;
    std::mutex zero_state_cache_mutex_;
    
    std::atomic<bool> in_use_{false};

protected:
    struct ProtectedMarker{};

public:
    CddContext(ProtectedMarker, std::unique_ptr<DataReader>&& reader, std::unique_ptr<DataWriter>&& writer,
               std::string backend_type, std::string mode);

    std::string_view get_backend_type() const { return backend_type_; }
    std::string_view get_mode() const { return mode_; }

private:
    std::string backend_type_;
    std::string mode_;
};

} // namespace cryptodd::ffi
