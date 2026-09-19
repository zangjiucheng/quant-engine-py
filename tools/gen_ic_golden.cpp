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

    const auto r = qe::analytics::ic_analysis(factor, returns, horizon);

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

    const std::string out = argc > 1 ? argv[1] : "tests/ic_golden.json";
    std::ofstream     f(out);
    if (!f) {
        std::cerr << "cannot write " << out << "\n";
        return 1;
    }
    f << j.dump(1) << "\n";
    std::cerr << "wrote " << out << " (engine " << qe::KVersion << ", n_obs " << r.n_obs << ")\n";
    return 0;
}
