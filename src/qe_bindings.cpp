/// @file src/qe_bindings.cpp
/// @brief PER-239 — the nanobind module.
///
/// S1 bound `ic_analysis` alone, to have something real for the parity
/// harness. S5 (PER-243) adds the rest of the standalone analytics:
/// `rolling_ic_analysis`, `long_short_backtest`, and the free functions
/// in `metrics.hpp`. All of them take plain series and return plain
/// results — no config machinery, no provenance, no engine hot loop —
/// which is why they are the cheapest surface in the epic and why they
/// come before S3/S4.
///
/// The `metrics.hpp` functions take `std::span<const EquityPoint>`.
/// They are bound over PARALLEL ARRAYS (timestamps, equity) rather than
/// by exposing `EquityPoint` itself: the caller side of this API is
/// numpy, the engine's own storage rule is struct-of-arrays, and a
/// bound list-of-structs would be the one shape neither side wants. The
/// temporary vector that reconstructs is not on any hot path.
///
/// Nothing here may take a per-bar callback. See PER-244: a Python
/// frame between two bars takes the GIL ~2500 times per backtest and
/// voids every number in CLAUDE.md's performance table.

#include "qe/analytics/ic.hpp"
#include "qe/data/multi_series.hpp"
#include "qe/data/series.hpp"
#include "qe/analytics/long_short.hpp"
#include "qe/analytics/metrics.hpp"
#include "qe/analytics/rolling_ic.hpp"
#include "qe/backtest/portfolio.hpp"
#include "qe/version.hpp"

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>

#include <string>

namespace nb = nanobind;

namespace {

/// Explain a failed `BarSeries::invariants_ok()`.
///
/// The BOOL is authoritative — this only turns it into a sentence. The
/// two are deliberately separate: a diagnostic that could disagree with
/// the gate would eventually be trusted over it. When nothing here
/// matches, say so rather than inventing a cause.
[[nodiscard]] std::string qe_bars_diagnosis(const qe::data::BarSeries& s) {
    const std::size_t n = s.close.size();
    auto              ragged = [&](const char* name, std::size_t got) {
        return std::string{"column '"} + name + "' has " + std::to_string(got) +
               " rows but 'close' has " + std::to_string(n) +
               " — every column of a bar series is the same length by construction";
    };
    if (s.timestamps.size() != n) {
        return ragged("timestamps", s.timestamps.size());
    }
    if (s.open.size() != n) {
        return ragged("open", s.open.size());
    }
    if (s.high.size() != n) {
        return ragged("high", s.high.size());
    }
    if (s.low.size() != n) {
        return ragged("low", s.low.size());
    }
    if (s.volume.size() != n) {
        return ragged("volume", s.volume.size());
    }
    for (std::size_t i = 1; i < n; ++i) {
        if (s.timestamps[i] < s.timestamps[i - 1]) {
            return "timestamps decrease at index " + std::to_string(i) + " (" +
                   std::to_string(s.timestamps[i - 1]) + " -> " + std::to_string(s.timestamps[i]) +
                   "). The engine fills at bar i+1's open and assumes an ordered grid; an "
                   "unordered series breaks that silently and nothing downstream can detect it. "
                   "Equal adjacent timestamps are permitted, decreasing ones are not.";
        }
    }
    return "BarSeries::invariants_ok() failed for a reason this diagnostic does not model";
}

}  // namespace

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

    // ---- bars: the SoA layout, as numpy views (S3 / PER-241) -------
    //
    // `BarSeries` is parallel std::vector<double> by architectural rule
    // — CLAUDE.md "Struct-of-Arrays, always" — which is exactly what
    // numpy wants. These are VIEWS over the engine's own buffers, not
    // copies.
    //
    // `rv_policy::reference_internal` states the intent: the array
    // borrows, and the owner must outlive it.
    //
    // HONESTLY: it could not be shown to be load-bearing. Removing it
    // from `.close` and rebuilding leaves every test green, including a
    // weakref probe that decides ownership rather than inferring it —
    // nanobind appears to tie the parent for property returns either
    // way. The policy stays because it is the documented way to ask for
    // what this code needs and costs nothing, NOT because a test proved
    // its absence breaks anything. Do not read the test below as
    // covering it.
    //
    // What the test does cover is the PROPERTY — owner alive while a
    // view is held, released after — which is what callers depend on
    // whichever mechanism supplies it.
    //
    // Read-only (`const double`): the engine's buffers are not Python's
    // to write through, and a per-bar mutation surface is the hot-loop
    // boundary of PER-244 wearing a different hat.
    using Arr64  = nb::ndarray<nb::numpy, const double, nb::ndim<1>>;
    using ArrI64 = nb::ndarray<nb::numpy, const std::int64_t, nb::ndim<1>>;

    auto col = [](const std::vector<double>& v) {
        return Arr64(v.data(), {v.size()});
    };
    auto col_i64 = [](const std::vector<std::int64_t>& v) {
        return ArrI64(v.data(), {v.size()});
    };

    // `is_weak_referenceable` so the ownership contract below can be
    // TESTED rather than asserted. Without it the first version of
    // `test_a_view_outlives_its_owner` passed with the keep-alive
    // policy REMOVED — it was reading freed-but-not-yet-reused memory
    // and calling that success.
    nb::class_<qe::data::BarSeries>(m, "BarSeries", nb::is_weak_referenceable())
        .def(nb::init<>())
        .def_prop_ro("timestamps",
                     [col_i64](const qe::data::BarSeries& s) { return col_i64(s.timestamps); },
                     nb::rv_policy::reference_internal, "int64 ns since the Unix epoch.")
        .def_prop_ro("open", [col](const qe::data::BarSeries& s) { return col(s.open); },
                     nb::rv_policy::reference_internal)
        .def_prop_ro("high", [col](const qe::data::BarSeries& s) { return col(s.high); },
                     nb::rv_policy::reference_internal)
        .def_prop_ro("low", [col](const qe::data::BarSeries& s) { return col(s.low); },
                     nb::rv_policy::reference_internal)
        .def_prop_ro("close", [col](const qe::data::BarSeries& s) { return col(s.close); },
                     nb::rv_policy::reference_internal)
        .def_prop_ro("volume", [col](const qe::data::BarSeries& s) { return col(s.volume); },
                     nb::rv_policy::reference_internal)
        .def("__len__", &qe::data::BarSeries::size)
        .def_static(
            "from_arrays",
            [](const std::vector<std::int64_t>& timestamps, const std::vector<double>& open,
               const std::vector<double>& high, const std::vector<double>& low,
               const std::vector<double>& close, const std::vector<double>& volume) {
                qe::data::BarSeries s;
                s.timestamps = timestamps;
                s.open       = open;
                s.high       = high;
                s.low        = low;
                s.close      = close;
                s.volume     = volume;
                // The engine's OWN validator decides. Writing a second
                // set of checks here would be a second opinion that can
                // drift from the one the engine actually enforces.
                if (!s.invariants_ok()) {
                    throw nb::value_error(qe_bars_diagnosis(s).c_str());
                }
                return s;
            },
            nb::arg("timestamps"), nb::arg("open"), nb::arg("high"), nb::arg("low"),
            nb::arg("close"), nb::arg("volume"),
            "Build from columns. Refuses ragged columns and out-of-order "
            "timestamps: unordered bars break the engine's look-ahead "
            "guarantee silently, and nothing downstream can detect it.");

    nb::class_<qe::data::MultiBarSeries>(m, "MultiBarSeries")
        .def(nb::init<>())
        .def_ro("symbols", &qe::data::MultiBarSeries::symbols)
        .def_prop_ro("timestamps",
                     [col_i64](const qe::data::MultiBarSeries& s) { return col_i64(s.timestamps); },
                     nb::rv_policy::reference_internal,
                     "The aligned master grid. EMPTY until align_intersection() runs — "
                     "before that the per-symbol grids may legitimately differ.")
        .def_prop_ro("bars",
                     [](qe::data::MultiBarSeries& s) { return &s.bars; },
                     nb::rv_policy::reference_internal,
                     "Symbol-major: bars[i] belongs to symbols[i].")
        .def("symbol_index", &qe::data::MultiBarSeries::symbol_index, nb::arg("name"),
             "Index of `name`, or None. The engine's hot loop uses indices and never "
             "names; this is the lookup you do ONCE.")
        .def("symbol_count", &qe::data::MultiBarSeries::symbol_count)
        .def("bar_count", &qe::data::MultiBarSeries::bar_count)
        .def("__len__", &qe::data::MultiBarSeries::symbol_count)
        .def(
            "align_intersection",
            [](qe::data::MultiBarSeries& s) {
                auto r = s.align_intersection();
                if (!r) {
                    throw nb::value_error(r.error().message.c_str());
                }
            },
            "Reduce every symbol to the common timestamp grid. Raises rather than "
            "returning a partial alignment.")
        .def_static(
            "from_series",
            [](const std::vector<std::string>&      symbols,
               const std::vector<qe::data::BarSeries>& series) {
                if (symbols.size() != series.size()) {
                    throw nb::value_error(
                        "symbols and series must be the same length — symbol-major "
                        "indexing is the engine's whole addressing scheme, and a "
                        "shorter zip would attach the right bars to the wrong name");
                }
                qe::data::MultiBarSeries mb;
                for (std::size_t i = 0; i < symbols.size(); ++i) {
                    mb.add_symbol(symbols[i], series[i]);
                }
                if (!mb.invariants_ok()) {
                    throw nb::value_error("MultiBarSeries invariants failed after assembly");
                }
                return mb;
            },
            nb::arg("symbols"), nb::arg("series"));

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

    // ---- rolling IC (EPIC-54) ---------------------------------------
    nb::class_<qe::analytics::RollingIcWindow>(m, "RollingIcWindow")
        .def_ro("open_bar", &qe::analytics::RollingIcWindow::open_bar)
        .def_ro("close_bar", &qe::analytics::RollingIcWindow::close_bar)
        .def_ro("open_ts_ns", &qe::analytics::RollingIcWindow::open_ts_ns)
        .def_ro("close_ts_ns", &qe::analytics::RollingIcWindow::close_ts_ns)
        .def_ro("n_obs", &qe::analytics::RollingIcWindow::n_obs)
        .def_ro("xs_ic_mean", &qe::analytics::RollingIcWindow::xs_ic_mean)
        .def_ro("xs_ic_std", &qe::analytics::RollingIcWindow::xs_ic_std)
        .def_ro("xs_ic_t_stat", &qe::analytics::RollingIcWindow::xs_ic_t_stat)
        .def_ro("xs_ic_t_stat_nw", &qe::analytics::RollingIcWindow::xs_ic_t_stat_nw)
        .def_ro("xs_ic_t_stat_nonoverlap",
                &qe::analytics::RollingIcWindow::xs_ic_t_stat_nonoverlap)
        .def_ro("ts_ic_mean", &qe::analytics::RollingIcWindow::ts_ic_mean)
        .def_ro("ts_ic_std", &qe::analytics::RollingIcWindow::ts_ic_std);

    nb::class_<qe::analytics::RollingIcResult>(m, "RollingIcResult")
        .def_ro("n_bars", &qe::analytics::RollingIcResult::n_bars)
        .def_ro("horizon", &qe::analytics::RollingIcResult::horizon)
        .def_ro("window_bars", &qe::analytics::RollingIcResult::window_bars)
        .def_ro("step_bars", &qe::analytics::RollingIcResult::step_bars)
        .def_ro("windows", &qe::analytics::RollingIcResult::windows);

    m.def("rolling_ic_analysis", &qe::analytics::rolling_ic_analysis, nb::arg("factor"),
          nb::arg("returns"), nb::arg("timestamps"), nb::arg("horizon"), nb::arg("window_bars"),
          nb::arg("step_bars"),
          "IC over a sliding window. Degenerate inputs (shape mismatch, "
          "window_bars > n_bars, any of horizon/step_bars == 0, window_bars < 2) "
          "return an empty `windows` rather than throwing.");

    // ---- long/short quantile backtest (EPIC-53) ---------------------
    nb::class_<qe::analytics::LongShortResult>(m, "LongShortResult")
        .def_ro("equity", &qe::analytics::LongShortResult::equity)
        .def_ro("period_returns", &qe::analytics::LongShortResult::period_returns)
        .def_ro("period_open_ts", &qe::analytics::LongShortResult::period_open_ts)
        .def_ro("total_return", &qe::analytics::LongShortResult::total_return)
        .def_ro("sharpe", &qe::analytics::LongShortResult::sharpe)
        // NEGATIVE here, unlike metrics.max_drawdown which is positive.
        // See the note on that function.
        .def_ro("max_dd", &qe::analytics::LongShortResult::max_dd)
        .def_ro("turnover", &qe::analytics::LongShortResult::turnover)
        .def_ro("n_periods", &qe::analytics::LongShortResult::n_periods)
        .def_ro("n_skipped", &qe::analytics::LongShortResult::n_skipped);

    m.def("long_short_backtest", &qe::analytics::long_short_backtest, nb::arg("factor"),
          nb::arg("returns"), nb::arg("timestamps"), nb::arg("quantiles"),
          nb::arg("rebalance_bars"), nb::arg("holding_periods_per_year"),
          nb::arg("commission_bps") = 0.0,
          "Top-minus-bottom quantile portfolio. `n_skipped` counts rebalance "
          "bars dropped for NaN or too few names — read it before reading `sharpe`.");

    // ---- metrics.hpp ------------------------------------------------
    m.def(
        "kahan_sum",
        [](const std::vector<double>& xs) { return qe::analytics::kahan_sum(xs); },
        nb::arg("xs"),
        "Compensated summation. The engine uses it for P&L over series longer "
        "than 100k bars, where naive accumulation loses cents.");

    m.def(
        "sharpe",
        [](const std::vector<double>& rets, int periods_per_year, double rf_annual) {
            return qe::analytics::sharpe(
                qe::analytics::ReturnSeries{rets, periods_per_year}, rf_annual);
        },
        nb::arg("rets"), nb::arg("periods_per_year") = 252, nb::arg("rf_annual") = 0.0);

    m.def(
        "sortino",
        [](const std::vector<double>& rets, int periods_per_year, double rf_annual) {
            return qe::analytics::sortino(
                qe::analytics::ReturnSeries{rets, periods_per_year}, rf_annual);
        },
        nb::arg("rets"), nb::arg("periods_per_year") = 252, nb::arg("rf_annual") = 0.0);

    // Parallel arrays in, EquityPoint reconstructed here. See the file
    // header for why the struct is not exposed.
    auto as_equity = [](const std::vector<std::int64_t>& ts, const std::vector<double>& eq) {
        if (ts.size() != eq.size()) {
            throw nb::value_error(
                "timestamps and equity must be the same length — a silent zip to the "
                "shorter one would misdate every point after the first gap");
        }
        std::vector<qe::backtest::EquityPoint> pts;
        pts.reserve(ts.size());
        for (std::size_t i = 0; i < ts.size(); ++i) {
            pts.push_back({ts[i], eq[i]});
        }
        return pts;
    };

    m.def(
        "total_return",
        [as_equity](const std::vector<std::int64_t>& timestamps_ns,
                    const std::vector<double>&       equity) {
            const auto pts = as_equity(timestamps_ns, equity);
            return qe::analytics::total_return(pts);
        },
        nb::arg("timestamps_ns"), nb::arg("equity"),
        "NaN for fewer than two points — not 0.0, which would read as a flat "
        "curve rather than as no answer.");

    m.def(
        "cagr",
        [as_equity](const std::vector<std::int64_t>& timestamps_ns,
                    const std::vector<double>& equity, int periods_per_year) {
            const auto pts = as_equity(timestamps_ns, equity);
            return qe::analytics::cagr(pts, periods_per_year);
        },
        nb::arg("timestamps_ns"), nb::arg("equity"), nb::arg("periods_per_year") = 252);

    m.def(
        "max_drawdown",
        [as_equity](const std::vector<std::int64_t>& timestamps_ns,
                    const std::vector<double>&       equity) {
            const auto pts = as_equity(timestamps_ns, equity);
            return qe::analytics::max_drawdown(pts);
        },
        nb::arg("timestamps_ns"), nb::arg("equity"),
        "Max peak-to-trough drawdown as a POSITIVE fraction: 0.25 means the "
        "curve gave back a quarter from its high. NaN below two points; 0.0 "
        "for a monotone curve.\n\n"
        "Note the sign clash inside qe::analytics, which is not this "
        "binding's invention: `metrics.hpp::max_drawdown` documents a "
        "positive fraction, while `LongShortResult.max_dd` documents the "
        "most NEGATIVE excursion and reports e.g. -0.202. Same namespace, "
        "opposite conventions. Compare like with like.");
}
