#include <pybind11/pybind11.h>
#include "zstd_compressor.h"

namespace py = pybind11;

PYBIND11_EXPORT int add(py::bytes i, int j) {
    using namespace cryptodd;
    std::unique_ptr<ICompressor> tmp = std::make_unique<cryptodd::ZstdCompressor>();
    std::string_view string_view = i;
    auto data = string_view.data();
    auto p = reinterpret_cast<const uint8_t*>(data);
    auto v = tmp->compress(std::span(p, string_view.size()));
    return v.size() + j;
}

PYBIND11_MODULE(cryptodd_arrays_cpp, m, py::mod_gil_not_used()) {
    m.doc() = "pybind11 example plugin"; // optional module docstring

    m.def("add", &add, "A function that adds two numbers");
}