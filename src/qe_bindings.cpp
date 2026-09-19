/// @file src/qe_bindings.cpp
/// @brief PER-239 — the nanobind module.
///
/// Scope is deliberately one function. S1 is about the repo, the
/// version alignment and the parity harness; the surface itself is
/// S3 (numpy marshalling), S4 (the .qe path) and S5 (analytics).
/// `ic_analysis` is here because S1's parity test needs something real
/// to compare, and because it is the function the nanobind-vs-pybind11
/// measurement was made on.
///
/// Nothing here may take a per-bar callback. See PER-244: a Python
/// frame between two bars takes the GIL ~2500 times per backtest and
/// voids every number in CLAUDE.md's performance table.

#include "qe/analytics/ic.hpp"
#include "qe/version.hpp"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <string>

namespace nb = nanobind;

NB_MODULE(_core, m) {
    m.doc() = "quant-engine bindings (nanobind). Python drives; Python does not participate.";

    // Baked in at configure time from the SUBMODULE's own project()
    // version and git HEAD, so they cannot disagree with the code that
    // was compiled. `qe/__init__.py` asserts against these at import.
    // `qe::KVersion` rather than a CMake define: the engine generates
    // it from its own PROJECT_VERSION onto target `qe`'s public include
    // path, and its header says why — eight hand-maintained literals
    // once drifted far enough that qe_daemon shipped 0.2.3-dev five
    // releases after 0.2.3. Reading the generated constant means this
    // binding cannot become the ninth.
    m.attr("__engine_version__")  = std::string(qe::KVersion);
    m.attr("__engine_commit__")   = QE_ENGINE_COMMIT;
    m.attr("__engine_describe__") = QE_ENGINE_DESCRIBE;

    nb::class_<qe::analytics::IcResult>(m, "IcResult")
        .def_ro("n_bars", &qe::analytics::IcResult::n_bars)
        .def_ro("n_symbols", &qe::analytics::IcResult::n_symbols)
        .def_ro("horizon", &qe::analytics::IcResult::horizon)
        .def_ro("ts_ic_mean", &qe::analytics::IcResult::ts_ic_mean)
        .def_ro("ts_ic_std", &qe::analytics::IcResult::ts_ic_std)
        .def_ro("xs_ic_mean", &qe::analytics::IcResult::xs_ic_mean)
        .def_ro("xs_ic_std", &qe::analytics::IcResult::xs_ic_std)
        .def_ro("xs_ic_t_stat", &qe::analytics::IcResult::xs_ic_t_stat)
        .def_ro("xs_ic_se_nw", &qe::analytics::IcResult::xs_ic_se_nw)
        .def_ro("xs_ic_nw_lag", &qe::analytics::IcResult::xs_ic_nw_lag)
        .def_ro("xs_ic_t_stat_nw", &qe::analytics::IcResult::xs_ic_t_stat_nw)
        .def_ro("xs_ic_t_stat_nonoverlap", &qe::analytics::IcResult::xs_ic_t_stat_nonoverlap)
        .def_ro("xs_ic_nonoverlap_phases", &qe::analytics::IcResult::xs_ic_nonoverlap_phases)
        .def_ro("n_obs", &qe::analytics::IcResult::n_obs)
        .def_ro("xs_ic_series", &qe::analytics::IcResult::xs_ic_series)
        .def_ro("ts_index", &qe::analytics::IcResult::ts_index);

    m.def("ic_analysis", &qe::analytics::ic_analysis, nb::arg("factor"), nb::arg("returns"),
          nb::arg("horizon"),
          "Cross-sectional and time-series IC. `factor` and `returns` are "
          "[n_symbols][n_bars]; mismatched shapes yield n_obs == 0 rather than throwing.");
}
