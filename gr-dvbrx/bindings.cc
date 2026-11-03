#include "frontend_cc.h"

#include <pybind11/pybind11.h>

namespace py = pybind11;

PYBIND11_MODULE(_gr, m)
{
    py::module::import("gnuradio.gr");

    using gr::dvbrx::frontend_cc;
    py::class_<frontend_cc, gr::block, gr::basic_block, std::shared_ptr<frontend_cc>>(
        m, "frontend_cc")
        .def(py::init(&frontend_cc::make),
             py::arg("sps"),
             py::arg("rolloff"),
             py::arg("rrc_delay") = 10,
             py::arg("loop_bw") = 1e-3,
             py::arg("damping") = 1.0)
        .def("gain", &frontend_cc::gain);
}
