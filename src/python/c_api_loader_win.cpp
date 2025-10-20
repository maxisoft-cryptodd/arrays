// Define WIN32_LEAN_AND_MEAN to exclude rarely-used APIs from Windows headers,
// speeding up compilation.
#define WIN32_LEAN_AND_MEAN
#include <iostream>
#include <windows.h>

#include <string>
#include <stdexcept>
#include <filesystem>

#ifndef PYTHON_LIBRARY_BUILD
#define PYTHON_LIBRARY_BUILD
#endif

// Include the canonical C API header to get the correct function signatures and types.
#include "cryptodd/c_api.h"
#include "c_api_loader.h"

// Use modern 'using' aliases for function pointer types. This is more readable
// and consistent with modern C++ practices.
using cdd_context_create_t = decltype(&cdd_context_create);
using cdd_context_destroy_t = decltype(&cdd_context_destroy);
using cdd_execute_op_t = decltype(&cdd_execute_op);
using cdd_error_message_t = decltype(&cdd_error_message);

static std::string s_module_path = {};

namespace fs = std::filesystem;
/**
 * @brief A singleton class to manage loading the cryptodd_arrays DLL and
 *        providing access to its C API functions.
 */
class CApiLoader {

public:
    /**
     * @brief Gets the singleton instance of the loader.
     *
     * The first time this is called, it will attempt to load the DLL and
     * resolve all function pointers. Subsequent calls return the existing instance.
     *
     * @return A reference to the loaded C API interface.
     * @throws std::runtime_error if the DLL or any of its functions cannot be loaded.
     */
    static const CApiLoader& get_instance() {
        static CApiLoader instance;
        return instance;
    }

    // Deleted copy and move constructors to enforce singleton pattern
    CApiLoader(const CApiLoader&) = delete;
    CApiLoader& operator=(const CApiLoader&) = delete;
    CApiLoader(CApiLoader&&) = delete;
    CApiLoader& operator=(CApiLoader&&) = delete;

    // Publicly accessible function pointers
    cdd_context_create_t cdd_context_create = nullptr;
    cdd_context_destroy_t cdd_context_destroy = nullptr;
    cdd_execute_op_t cdd_execute_op = nullptr;
    cdd_error_message_t cdd_error_message = nullptr;

private:
    HMODULE dll_handle_ = nullptr;

    /**
     * @brief Private constructor that handles loading the DLL and its functions.
     */
    CApiLoader() {
        fs::path dll_name = "cryptodd_arrays.dll";
        if (!s_module_path.empty())
        {
            if (auto path = fs::path(s_module_path).parent_path() / dll_name; fs::exists(path))
            {
                dll_name = path.string();
            }
        }

        dll_handle_ = LoadLibraryW(dll_name.c_str());
        if (!dll_handle_) {
            throw std::runtime_error("Failed to load " + dll_name.string() + ". Error code: " + std::to_string(GetLastError()));
        }

        // Helper lambda to get a function address and throw on failure
        auto get_proc = [&](const char* func_name) {
            FARPROC proc = GetProcAddress(dll_handle_, func_name);
            if (!proc) {
                FreeLibrary(dll_handle_); // Clean up on failure
                throw std::runtime_error("Failed to get address of function: " + std::string(func_name));
            }
            return proc;
        };

        cdd_context_create = reinterpret_cast<cdd_context_create_t>(get_proc("cdd_context_create"));
        cdd_context_destroy = reinterpret_cast<cdd_context_destroy_t>(get_proc("cdd_context_destroy"));
        cdd_execute_op = reinterpret_cast<cdd_execute_op_t>(get_proc("cdd_execute_op"));
        cdd_error_message = reinterpret_cast<cdd_error_message_t>(get_proc("cdd_error_message"));
    }

    /**
     * @brief Destructor that frees the loaded DLL.
     */
    ~CApiLoader() {
        if (dll_handle_) {
            FreeLibrary(dll_handle_);
        }
    }
};

namespace cryptodd::c_api
{
    void setup(std::string_view module_path)
    {
        s_module_path = module_path;
    }
}

// C-style wrapper functions that will be called from other parts of the code.
// They use the singleton loader to access the real functions from the DLL.
extern "C" {

    cdd_handle_t cdd_context_create(const char* json_config, size_t config_len) {
        return CApiLoader::get_instance().cdd_context_create(json_config, config_len);
    }

    int64_t cdd_context_destroy(cdd_handle_t handle) {
        return CApiLoader::get_instance().cdd_context_destroy(handle);
    }

    int64_t cdd_execute_op(cdd_handle_t handle, const char* json_op_request, size_t request_len, const void* input_data_ptr, int64_t input_data_bytes, void* output_data_ptr, int64_t max_output_data_bytes, char* json_op_response, size_t max_json_response_bytes) {
        return CApiLoader::get_instance().cdd_execute_op(handle, json_op_request, request_len, input_data_ptr, input_data_bytes, output_data_ptr, max_output_data_bytes, json_op_response, max_json_response_bytes);
    }

    const char* cdd_error_message(int64_t error_code) {
        return CApiLoader::get_instance().cdd_error_message(error_code);
    }

} // extern "C"