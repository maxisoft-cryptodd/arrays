#pragma once

#include <stddef.h>
#include <stdint.h>
#ifdef STATIC_LIBRARY_BUILD
#include "cryptodd_arrays_lib_export.h"
#endif
#ifdef SHARED_LIBRARY_BUILD
#include "cryptodd_arrays_shared_export.h"
#endif

#ifdef CRYPTODD_ARRAYS_SHARED_EXPORT
#define CRYPTODD_API CRYPTODD_ARRAYS_SHARED_EXPORT
#elifdef  CRYPTODD_ARRAYS_LIB_EXPORT
#define CRYPTODD_API CRYPTODD_ARRAYS_LIB_EXPORT
#elifdef PYTHON_LIBRARY_BUILD
#define CRYPTODD_API
#elifdef _WIN32
#ifdef CRYPTODD_DLL_EXPORTS
#define CRYPTODD_API __declspec(dllexport)
#else
#define CRYPTODD_API __declspec(dllimport)
#endif
#else
#define CRYPTODD_API __attribute__((visibility("default")))
#endif


#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to a CddContext instance.
// Using a specific type for clarity and to avoid potential conflicts with intptr_t.
    typedef int64_t cdd_handle_t;

// Error Codes
enum {
    CDD_SUCCESS = 0,
    CDD_ERROR_UNKNOWN = -1,
    CDD_ERROR_INVALID_JSON = -2,
    CDD_ERROR_INVALID_HANDLE = -3,
    CDD_ERROR_OPERATION_FAILED = -4,
    CDD_ERROR_RESPONSE_BUFFER_TOO_SMALL = -5,
    CDD_ERROR_INVALID_ARGUMENT = -6,
    CDD_ERROR_RESOURCE_UNAVAILABLE = -7,
};

/**
 * @brief Creates a context for interacting with a .cdd resource.
 *
 * @param json_config A UTF-8 encoded JSON string configuring the backend.
 * @param config_len Length of the JSON string in bytes.
 * @return cdd_handle_t a positive integer handle on success, negative error code on failure.
 */
CRYPTODD_API cdd_handle_t cdd_context_create(
    const char* json_config,
    size_t config_len
);

/**
 * @brief Destroys a context and releases all associated resources.
 *
 * @param handle The context handle to destroy.
 * @return int64_t 0 on success, negative error code on failure.
 */
CRYPTODD_API int64_t cdd_context_destroy(cdd_handle_t handle);

/**
 * @brief Executes an operation on a given context.
 *
 * @param handle The context handle.
 * @param json_op_request A UTF-8 encoded JSON string describing the operation.
 * @param request_len Length of the JSON request string.
 * @param input_data_ptr Pointer to the source data buffer (used by 'Store' operations).
 * @param input_data_bytes Size of the input data buffer in bytes.
 * @param output_data_ptr Pointer to the destination buffer (used by 'Load' operations).
 * @param max_output_data_bytes Capacity of the output buffer in bytes.
 * @param json_op_response Buffer to write the UTF-8 encoded JSON response into. Must be large enough.
 * @param max_json_response_bytes Capacity of the JSON response buffer.
 * @return int64_t 0 on success, negative error code on failure. The details of the success or
 *         failure are written into the `json_op_response` buffer.
 */
CRYPTODD_API int64_t cdd_execute_op(
    cdd_handle_t handle,
    const char* json_op_request,
    size_t request_len,
    const void* input_data_ptr,
    int64_t input_data_bytes,
    void* output_data_ptr,
    int64_t max_output_data_bytes,
    char* json_op_response,
    size_t max_json_response_bytes
);

/**
 * @brief Translates an error code from the API into a human-readable string.
 *
 * @param error_code The negative error code returned by an API function.
 * @return const char* A static, null-terminated string describing the error. Returns "Unknown Error" for invalid codes.
 */
CRYPTODD_API const char* cdd_error_message(int64_t error_code);

#ifdef __cplusplus
}
#endif
