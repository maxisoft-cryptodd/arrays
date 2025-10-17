#include "include/cryptodd/c_api.h"
#include "c_api/cdd_context.h"

#include <nlohmann/json.hpp>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <string_view>
#include <memory>
#include <string>

namespace {
    std::mutex g_context_map_mutex;
    std::unordered_map<cdd_handle_t, std::unique_ptr<cryptodd::ffi::CddContext>> g_contexts;
    std::atomic<cdd_handle_t> g_next_handle{1}; // Start handles at 1, as 0 can be a sentinel value
}

// Helper to create a standardized error response JSON
static nlohmann::json create_error_json(int64_t code, const std::string& message) {
    nlohmann::json error_obj;
    error_obj["code_value"] = code;
    // We need a helper to get the code name from the value, to be added later
    error_obj["message"] = message;
    
    nlohmann::json response;
    response["status"] = "Error";
    response["error"] = error_obj;
    return response;
}

// Map int64_t code to user-friendly message string
static const char* error_code_to_message(int64_t code) {
    switch (code) {
        case CDD_SUCCESS: return "Operation completed successfully.";
        case CDD_ERROR_UNKNOWN: return "An unspecified internal error occurred.";
        case CDD_ERROR_INVALID_JSON: return "The provided JSON string was malformed or failed validation.";
        case CDD_ERROR_INVALID_HANDLE: return "The provided context handle is not valid or has been destroyed.";
        case CDD_ERROR_OPERATION_FAILED: return "The operation was valid but failed during execution.";
        case CDD_ERROR_RESPONSE_BUFFER_TOO_SMALL: return "The provided JSON response buffer is too small for the result.";
        case CDD_ERROR_INVALID_ARGUMENT: return "A function argument was invalid (e.g., null pointer).";
        case CDD_ERROR_RESOURCE_UNAVAILABLE: return "A required resource could not be accessed (e.g., file not found).";
        default: return "Unknown Error.";
    }
}

extern "C" {

CRYPTODD_API const char* cdd_error_message(int64_t error_code) {
    return error_code_to_message(error_code);
}

CRYPTODD_API int64_t cdd_context_create(const char* json_config, size_t config_len, cdd_handle_t* out_handle) {
    if (!json_config || !out_handle) {
        return CDD_ERROR_INVALID_ARGUMENT;
    }

    try {
        auto config = nlohmann::json::parse(std::string_view(json_config, config_len));
        
        auto context_result = cryptodd::ffi::CddContext::create(config);
        if (!context_result) {
            // A more granular error mapping could be added here later.
            // For now, RESOURCE_UNAVAILABLE is a good catch-all for file errors.
            return CDD_ERROR_RESOURCE_UNAVAILABLE;
        }
        
        cdd_handle_t handle = g_next_handle.fetch_add(1);
        {
            std::lock_guard lock(g_context_map_mutex);
            g_contexts[handle] = std::move(*context_result);
        }
        *out_handle = handle;
        return CDD_SUCCESS;

    } catch (const nlohmann::json::parse_error&) {
        return CDD_ERROR_INVALID_JSON;
    } catch (const std::exception&) {
        return CDD_ERROR_UNKNOWN;
    }
}

CRYPTODD_API int64_t cdd_context_destroy(cdd_handle_t handle) {
    if (handle == 0) {
        return CDD_ERROR_INVALID_HANDLE;
    }
    
    size_t erased_count;
    {
        std::lock_guard lock(g_context_map_mutex);
        erased_count = g_contexts.erase(handle);
    }
    
    return (erased_count > 0) ? CDD_SUCCESS : CDD_ERROR_INVALID_HANDLE;
}

CRYPTODD_API int64_t cdd_execute_op(
    cdd_handle_t handle,
    const char* json_op_request,
    size_t request_len,
    const void* input_data_ptr,
    int64_t input_data_bytes,
    void* output_data_ptr,
    int64_t max_output_data_bytes,
    char* json_op_response,
    size_t max_json_response_bytes)
{
    if (handle == 0 || !json_op_request || !json_op_response || max_json_response_bytes == 0) {
        return CDD_ERROR_INVALID_ARGUMENT;
    }

    int64_t final_status_code = CDD_ERROR_UNKNOWN;
    std::string response_str;

    try {
        nlohmann::json request_json = nlohmann::json::parse(std::string_view(json_op_request, request_len));

        std::unique_lock lock(g_context_map_mutex);
        auto it = g_contexts.find(handle);
        if (it == g_contexts.end()) {
            lock.unlock();
            response_str = create_error_json(CDD_ERROR_INVALID_HANDLE, cdd_error_message(CDD_ERROR_INVALID_HANDLE)).dump();
            final_status_code = CDD_ERROR_INVALID_HANDLE;
        } else {
            cryptodd::ffi::CddContext& context = *(it->second);
            lock.unlock(); // Release lock while the potentially long operation is running

            auto op_result = context.execute_operation(
                request_json,
                {static_cast<const std::byte*>(input_data_ptr), (size_t)input_data_bytes},
                {static_cast<std::byte*>(output_data_ptr), (size_t)max_output_data_bytes}
            );
            
            nlohmann::json final_response;
            if (op_result) {
                final_response = {{"status", "Success"}, {"result", *op_result}};
                final_status_code = CDD_SUCCESS;
            } else {
                final_response = create_error_json(CDD_ERROR_OPERATION_FAILED, op_result.error());
                final_status_code = CDD_ERROR_OPERATION_FAILED;
            }
            response_str = final_response.dump();
        }
    } catch (const nlohmann::json::parse_error& e) {
        response_str = create_error_json(CDD_ERROR_INVALID_JSON, e.what()).dump();
        final_status_code = CDD_ERROR_INVALID_JSON;
    } catch (const std::exception& e) {
        response_str = create_error_json(CDD_ERROR_UNKNOWN, e.what()).dump();
        final_status_code = CDD_ERROR_UNKNOWN;
    }

    if (response_str.length() + 1 > max_json_response_bytes) {
        return CDD_ERROR_RESPONSE_BUFFER_TOO_SMALL;
    }

    memcpy(json_op_response, response_str.c_str(), response_str.length() + 1);
    
    return final_status_code;
}

} // extern "C"
