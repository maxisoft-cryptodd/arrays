#include <dlfcn.h>      // Required for dlopen, dlsym, dlclose
#include <stdexcept>    // For std::runtime_error
#include <string>       // For std::string
#include <filesystem>

#ifndef PYTHON_LIBRARY_BUILD
#define PYTHON_LIBRARY_BUILD
#endif

// Include the C API header to get function declarations and typedefs
#include "cryptodd/c_api.h"
#include "c_api_loader.h"

// --- Function Pointer Type Definitions (Copied from c_api_loader_win.cpp logic) ---
using cdd_context_create_t = decltype(&cdd_context_create);
using cdd_context_destroy_t = decltype(&cdd_context_destroy);
using cdd_execute_op_t = decltype(&cdd_execute_op);
using cdd_error_message_t = decltype(&cdd_error_message);
// ----------------------------------------------------------------------------------

static std::string s_module_path = {};

namespace fs = std::filesystem;

class CApiLoader {
public:
    
    static const CApiLoader& get_instance() {
        static CApiLoader instance;
        return instance;
    }

    CApiLoader(const CApiLoader&) = delete;
    CApiLoader& operator=(const CApiLoader&) = delete;
    CApiLoader(CApiLoader&&) = delete;
    CApiLoader& operator=(CApiLoader&&) = delete;

    cdd_context_create_t cdd_context_create = nullptr;
    cdd_context_destroy_t cdd_context_destroy = nullptr;
    cdd_execute_op_t cdd_execute_op = nullptr;
    cdd_error_message_t cdd_error_message = nullptr;

private:
    void* handle_ = nullptr; // Use void* for dlopen handle

    CApiLoader() {
        // Shared libraries use .so on Linux and .dylib on macOS.
        // The standard linker name for 'cryptodd_arrays' should be libcryptodd_arrays.so/dylib
        const char* default_lib_name =
#ifdef __APPLE__
        default_lib_name = "libcryptodd_arrays.dylib";
#else
            "libcryptodd_arrays.so";
#endif

        fs::path lib_path = default_lib_name;
        if (!s_module_path.empty()) {
            if (auto path = fs::path(s_module_path).parent_path() / default_lib_name; fs::exists(path)) {
                lib_path = path;
            }
        }

        // RTLD_LAZY: Resolve symbols as needed.
        // RTLD_LOCAL: Symbols are not made available to other dynamically loaded libraries (best practice).
        handle_ = dlopen(lib_path.c_str(), RTLD_LAZY | RTLD_LOCAL);
        
        if (!handle_) {
            // dlerror provides the error message
            throw std::runtime_error("Failed to load " + lib_path.string() + 
                                     ". Error: " + std::string(dlerror()));
        }

        auto get_proc = [&](const char* func_name) -> void* {
            dlerror(); // Clear any existing error
            void* proc = dlsym(handle_, func_name);
            
            const char* error = dlerror();
            if (error != nullptr) {
                dlclose(handle_);
                throw std::runtime_error("Failed to get address of function: " + std::string(func_name) +
                                         ". Error: " + std::string(error));
            }
            return proc;
        };

        cdd_context_create = reinterpret_cast<cdd_context_create_t>(get_proc("cdd_context_create"));
        cdd_context_destroy = reinterpret_cast<cdd_context_destroy_t>(get_proc("cdd_context_destroy"));
        cdd_execute_op = reinterpret_cast<cdd_execute_op_t>(get_proc("cdd_execute_op"));
        cdd_error_message = reinterpret_cast<cdd_error_message_t>(get_proc("cdd_error_message"));
    }

    ~CApiLoader() {
        if (handle_) {
            dlclose(handle_);
        }
    }
};

namespace cryptodd::c_api {
    void setup(std::string_view module_path) {
        s_module_path = module_path;
    }
}

// --- Wrapper Functions (These must match the C API header signature) ---
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

}