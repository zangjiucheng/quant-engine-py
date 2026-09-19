/// @file tools/gen_ic_golden.cpp
/// @brief PER-239 — emit the parity golden, FROM THE ENGINE.
///
/// The bindings' acceptance is "the bound values match the C++". The
/// only way that means anything is if the reference comes out of the
/// C++ rather than out of a second implementation someone wrote in
/// Python to check the first one — the PER-221 lesson, where the gbdt
/// parity test asserts against xgboost's own output rather than a
/// second Python walk of the model.
///
/// Writes BOTH the input and the expected output, so the Python side
/// never has to reproduce a random stream. A shared RNG that drifts
/// between languages would turn a parity test into a coincidence test.
///
/// Deterministic by construction: a fixed-seed LCG written out in full,
/// not <random>, whose distributions are implementation-defined and
/// have differed between libstdc++ and libc++ before.

#include "qe/analytics/ic.hpp"
#include "qe/analytics/long_short.hpp"
#include "qe/analytics/metrics.hpp"
#include "qe/analytics/rolling_ic.hpp"
#include "qe/backtest/portfolio.hpp"
#include "qe/version.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

struct Lcg {
    std::uint64_t s;
    double        next() {  // uniform [0,1)
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(s >> 11) / 9007199254740992.0;
    }
};

}  // namespace

int main(int argc, char** argv) {
    const std::size_t n_sym = 9, n_bar = 400, horizon = 20;
    Lcg               rng{42};

    std::vector<std::vector<double>> factor(n_sym, std::vector<double>(n_bar));
    std::vector<std::vector<double>> returns(n_sym, std::vector<double>(n_bar));
    for (std::size_t s = 0; s < n_sym; ++s) {
        for (std::size_t t = 0; t < n_bar; ++t) {
            factor[s][t]  = rng.next() * 2.0 - 1.0;
            returns[s][t] = (rng.next() * 2.0 - 1.0) * 0.02;
        }
    }

    // Timestamps: one trading day apart, arbitrary epoch. Only their
    // ordering and spacing matter to the windowing.
    std::vector<std::int64_t> ts(n_bar);
    for (std::size_t t = 0; t < n_bar; ++t) {
        ts[t] = 1'600'000'000'000'000'000LL + static_cast<std::int64_t>(t) * 86'400'000'000'000LL;
    }

    const auto r  = qe::analytics::ic_analysis(factor, returns, horizon);
    const auto rr = qe::analytics::rolling_ic_analysis(factor, returns, ts, horizon,
                                                       /*window_bars=*/120, /*step_bars=*/20);
    const auto ls = qe::analytics::long_short_backtest(factor, returns, ts, /*quantiles=*/3,
                                                       /*rebalance_bars=*/21,
                                                       /*holding_periods_per_year=*/12.0,
                                                       /*commission_bps=*/5.0);

    // A synthetic equity curve for the metrics.hpp functions: they take
    // EquityPoint, and reusing the long/short equity would make the
    // metric golden depend on the long/short golden.
    std::vector<qe::backtest::EquityPoint> eq;
    std::vector<double>                    eq_vals;
    std::vector<std::int64_t>              eq_ts;
    std::vector<double>                    rets;
    {
        Lcg   r2{99};
        double level = 100.0;
        for (std::size_t t = 0; t < 250; ++t) {
            const double ret = (r2.next() * 2.0 - 1.0) * 0.01;
            rets.push_back(ret);
            level *= (1.0 + ret);
            const std::int64_t tt =
                1'600'000'000'000'000'000LL + static_cast<std::int64_t>(t) * 86'400'000'000'000LL;
            eq.push_back({tt, level});
            eq_ts.push_back(tt);
            eq_vals.push_back(level);
        }
    }

    nlohmann::json j;
    j["engine_version"]      = std::string{qe::KVersion};
    j["input"]["factor"]     = factor;
    j["input"]["returns"]    = returns;
    j["input"]["horizon"]    = horizon;
    j["expected"]            = {
        {"n_bars", r.n_bars},
        {"n_symbols", r.n_symbols},
        {"horizon", r.horizon},
        {"ts_ic_mean", r.ts_ic_mean},
        {"ts_ic_std", r.ts_ic_std},
        {"xs_ic_mean", r.xs_ic_mean},
        {"xs_ic_std", r.xs_ic_std},
        {"xs_ic_t_stat", r.xs_ic_t_stat},
        {"xs_ic_se_nw", r.xs_ic_se_nw},
        {"xs_ic_nw_lag", r.xs_ic_nw_lag},
        {"xs_ic_t_stat_nw", r.xs_ic_t_stat_nw},
        {"xs_ic_t_stat_nonoverlap", r.xs_ic_t_stat_nonoverlap},
        {"xs_ic_nonoverlap_phases", r.xs_ic_nonoverlap_phases},
        {"n_obs", r.n_obs},
        {"xs_ic_series", r.xs_ic_series},
        {"ts_index", r.ts_index},
    };

    j["input"]["timestamps"] = ts;
    j["rolling"]["params"]   = {{"window_bars", 120}, {"step_bars", 20}};
    j["rolling"]["expected"] = {{"n_bars", rr.n_bars},
                                {"horizon", rr.horizon},
                                {"window_bars", rr.window_bars},
                                {"step_bars", rr.step_bars},
                                {"n_windows", rr.windows.size()}};
    auto wins = nlohmann::json::array();
    for (const auto& w : rr.windows) {
        wins.push_back({{"open_bar", w.open_bar},
                        {"close_bar", w.close_bar},
                        {"open_ts_ns", w.open_ts_ns},
                        {"close_ts_ns", w.close_ts_ns},
                        {"n_obs", w.n_obs},
                        {"xs_ic_mean", w.xs_ic_mean},
                        {"xs_ic_std", w.xs_ic_std},
                        {"xs_ic_t_stat", w.xs_ic_t_stat},
                        {"xs_ic_t_stat_nw", w.xs_ic_t_stat_nw},
                        {"xs_ic_t_stat_nonoverlap", w.xs_ic_t_stat_nonoverlap},
                        {"ts_ic_mean", w.ts_ic_mean},
                        {"ts_ic_std", w.ts_ic_std}});
    }
    j["rolling"]["windows"] = wins;

    j["long_short"]["params"]   = {{"quantiles", 3},
                                   {"rebalance_bars", 21},
                                   {"holding_periods_per_year", 12.0},
                                   {"commission_bps", 5.0}};
    j["long_short"]["expected"] = {{"equity", ls.equity},
                                   {"period_returns", ls.period_returns},
                                   {"period_open_ts", ls.period_open_ts},
                                   {"total_return", ls.total_return},
                                   {"sharpe", ls.sharpe},
                                   {"max_dd", ls.max_dd},
                                   {"turnover", ls.turnover},
                                   {"n_periods", ls.n_periods},
                                   {"n_skipped", ls.n_skipped}};

    j["metrics"]["input"]    = {{"rets", rets}, {"timestamps_ns", eq_ts}, {"equity", eq_vals}};
    j["metrics"]["expected"] = {
        {"kahan_sum", qe::analytics::kahan_sum(rets)},
        {"sharpe", qe::analytics::sharpe(qe::analytics::ReturnSeries{rets, 252}, 0.0)},
        {"sortino", qe::analytics::sortino(qe::analytics::ReturnSeries{rets, 252}, 0.0)},
        {"total_return", qe::analytics::total_return(eq)},
        {"cagr", qe::analytics::cagr(eq, 252)},
        {"max_drawdown", qe::analytics::max_drawdown(eq)},
    };

    const std::string out = argc > 1 ? argv[1] : "tests/ic_golden.json";
    std::ofstream     f(out);
    if (!f) {
        std::cerr << "cannot write " << out << "\n";
        return 1;
    }
    f << j.dump(1) << "\n";
    std::cerr << "wrote " << out << " (engine " << qe::KVersion << ", ic n_obs " << r.n_obs
              << ", rolling windows " << rr.windows.size() << ", ls periods " << ls.n_periods
              << ")\n";
    return 0;
}
