#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>
#include "cryptodd/c_api.h"
#include <stdexcept>
#include <vector>
#include "c_api_loader.h"

namespace py = pybind11;

namespace
{
    size_t nbytes(const py::buffer_info& buffer_info)
    {
        return buffer_info.size * buffer_info.itemsize;
    }

    constexpr size_t MAX_RESPONSE_SIZE = 2 * 1024 * 1024; // 2MB limit
}

// Define a custom C++ exception that can be caught in Python
class CddException : public std::exception {
public:
    CddException(std::string msg, int64_t code, std::string response_json)
        : msg_(std::move(msg)), code_(code), response_json_(std::move(response_json)) {}
    const char* what() const noexcept override { return msg_.c_str(); }
    int64_t code() const { return code_; }
    const std::string& response_json() const { return response_json_; }
private:
    std::string msg_;
    int64_t code_;
    std::string response_json_;
};

class CddFileWrapper {
public:
    CddFileWrapper(const std::string& json_config) {
        handle_ = cdd_context_create(json_config.c_str(), json_config.length());
        if (handle_ <= 0) {
            // The response buffer is empty on creation failure, but we can get the error message.
            throw CddException("Failed to create context", handle_, cdd_error_message(handle_));
        }
    }
    ~CddFileWrapper() { close(); }
    void close() {
        if (handle_ > 0) {
            cdd_context_destroy(handle_);
            handle_ = 0;
        }
    }

    // THE REVISED WORKHORSE METHOD
    py::bytes _execute_op(py::object json_op, py::object input_data, py::object output_data) {
        std::string json_op_str;
        if (py::isinstance<py::str>(json_op)) {
            json_op_str = json_op.cast<std::string>();
        } else if (py::isinstance<py::bytes>(json_op)) {
            json_op_str = json_op.cast<std::string>(); // Assumes UTF-8
        } else {
            throw std::runtime_error("json_op must be str or bytes");
        }

        const void* input_ptr = nullptr;
        int64_t input_bytes = 0;
        py::buffer_info input_buf;
        if (!input_data.is_none()) {
            try {
                input_buf = py::buffer(input_data).request();
            } catch (const py::cast_error&) {
                throw std::runtime_error("input_data must be a buffer-protocol compatible object (like a numpy array).");
            }
            input_ptr = input_buf.ptr;
            input_bytes = nbytes(input_buf);
        }

        void* output_ptr = nullptr;
        int64_t max_output_bytes = 0;
        py::buffer_info output_buf;
        if (!output_data.is_none()) {
            try {
                 output_buf = py::buffer(output_data).request(true); // Writable
            } catch (const py::cast_error&) {
                throw std::runtime_error("output_data must be a writable buffer-protocol compatible object (like a numpy array).");
            }
            output_ptr = output_buf.ptr;
            max_output_bytes = nbytes(output_buf);
        }

        std::vector<char> response_buf(16384);
        int64_t status;

        {
            py::gil_scoped_release release;
            while (true) {
                status = cdd_execute_op(handle_, json_op_str.c_str(), json_op_str.length(),
                                      input_ptr, input_bytes,
                                      output_ptr, max_output_bytes,
                                      response_buf.data(), response_buf.size());

                if (status == CDD_ERROR_RESPONSE_BUFFER_TOO_SMALL) {
                    if (response_buf.size() >= MAX_RESPONSE_SIZE) {
                        // Re-acquire GIL to throw Python exception
                        py::gil_scoped_acquire acquire;
                        throw std::runtime_error("JSON response from C API exceeds 2MB limit.");
                    }
                    response_buf.resize(response_buf.size() * 2);
                    continue; // Retry
                }
                break;
            }
        } // GIL re-acquired

        if (status != CDD_SUCCESS) {
            throw CddException("Operation failed", status, std::string(response_buf.data()));
        }

        // Safely find the length of the null-terminated response string
        size_t response_len = strnlen(response_buf.data(), response_buf.size());

        // Return the raw JSON response as Python bytes
        return py::bytes(response_buf.data(), response_len);
    }

private:
    cdd_handle_t handle_{0};
};

PYBIND11_MODULE(cryptodd_arrays_cpp, m) {
    m.doc() = "Low-level C++ bridge for cryptodd-arrays.";
    // Register the custom exception
    PYBIND11_CONSTINIT static py::gil_safe_call_once_and_store<py::object> exc_storage;
    exc_storage.call_once_and_store_result(
        [&]() { return py::exception<CddException>(m, "CddException", PyExc_RuntimeError); });

    const auto module_path = m.attr("__file__").cast<std::string>();
    cryptodd::c_api::setup(module_path);

    py::register_exception_translator([](std::exception_ptr p) {
        try {
            if (p) std::rethrow_exception(p);
        } catch (const CddException &e) {
            // Create a Python CddException and set its attributes
            py::object py_exc = exc_storage.get_stored()();
            py_exc.attr("response_json") = e.response_json();
            py_exc.attr("code") = e.code();
            py_exc.attr("code_message") = py::str(cdd_error_message(e.code()));
            py::set_error(exc_storage.get_stored(), py_exc);
        }
    });

    py::class_<CddFileWrapper>(m, "_CddFile")
        .def(py::init<const std::string&>(), py::arg("json_config"))
        .def("close", &CddFileWrapper::close)
        .def("_execute_op", &CddFileWrapper::_execute_op, py::arg("json_op"), py::arg("input_data"),
             py::arg("output_data"))
        .def("__enter__", [](py::object self) { return self; })
        .def("__exit__", [](CddFileWrapper& self, py::object, py::object, py::object) { self.close(); });
}