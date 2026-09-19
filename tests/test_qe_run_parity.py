"""PER-242 / S4 — the parity test S1 could not hold.

S1's stated acceptance was "one `.qe` through both `qe_run` and the
bindings, results identical". That was circular at S1 time: running a
`.qe` through the bindings is what S4 adds. It is possible now, so it
lands here.

`qe_run` is built from the PINNED SUBMODULE, not from a developer's own
quant-engine checkout — that one is on `main` while the submodule is on
a tag, and comparing across two engines would call every version
difference a binding bug. `tools/build-qe-run.sh` does it.
"""

import contextlib
import json
import os
import pathlib
import subprocess
import unittest

import qe

@contextlib.contextmanager
def chdir(path):
    prev = os.getcwd()
    os.chdir(path)
    try:
        yield
    finally:
        os.chdir(prev)


REPO = pathlib.Path(__file__).resolve().parents[1]
ENGINE = REPO / "extern" / "quant-engine"
QE_RUN = REPO / "build" / "qe_run" / "bin" / "qe_run"
FIXTURE = "tests/fixtures/qe_run_smoke.qe"
RESULTS = ENGINE / "out" / "qe_run_smoke_qe_results.json"


@unittest.skipUnless(
    QE_RUN.exists(),
    f"{QE_RUN} not built — run tools/build-qe-run.sh (this is S1's acceptance test; "
    f"skipping it means the bindings' agreement with the engine is unverified)",
)
class QeRunParity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        (ENGINE / "out").mkdir(exist_ok=True)
        RESULTS.unlink(missing_ok=True)
        r = subprocess.run([str(QE_RUN), FIXTURE], cwd=ENGINE, capture_output=True, text=True)
        if r.returncode != 0:
            raise AssertionError(f"qe_run exited {r.returncode}\n{r.stderr[-2000:]}")
        cls.cli = json.loads(RESULTS.read_text())

        # Same CWD as the CLI run above, deliberately: the fixture's
        # `file(...)` path is relative, so resolving it from anywhere
        # else compares two different inputs — or, as it did first
        # time, fails to resolve at all.
        with chdir(ENGINE):
            cfg = qe.load_qe(FIXTURE)
            cls.bound = qe.run_backtest(cfg, qe.load_bars(cfg))

    def test_the_cli_and_the_bindings_ran_the_same_engine(self):
        # Both must be the pinned tag. If the CLI were the developer's
        # `main` build, every assertion below would be comparing two
        # engines and blaming the binding for the difference.
        self.assertEqual(self.cli["provenance"]["engine"]["version"], qe.engine_version)

    def test_equity_curve_is_identical_point_for_point(self):
        cli = self.cli["equity_curve"]
        bound = self.bound.combined_equity
        self.assertEqual(len(cli), len(bound))
        self.assertGreater(len(cli), 10, "a two-point curve would pass this vacuously")
        for i, (c, b) in enumerate(zip(cli, bound)):
            with self.subTest(point=i):
                self.assertEqual(c["t"], b.timestamp_ns)
                self.assertEqual(c["equity"], b.equity)

    def test_trade_count_matches(self):
        self.assertEqual(
            len(self.cli["trades"]), sum(p.n_trades for p in self.bound.per_strategy)
        )
        self.assertGreater(len(self.cli["trades"]), 0, "a no-trade run tests nothing")

    def test_capital_matches_the_config(self):
        # results.json's `run` block carries timing, not capital, so the
        # config is the common reference for both sides.
        with chdir(ENGINE):
            cfg = qe.load_qe(FIXTURE)
        self.assertEqual(cfg.capital, self.bound.total_capital)
        self.assertEqual(self.cli["equity_curve"][0]["equity"], self.bound.total_capital)

    # ---- the provenance invariant, made mechanical ------------------

    def test_qe_run_emits_provenance_and_the_binding_does_not(self):
        """PER-226's rule, as an assertion rather than a comment.

        `provenance.hpp:41-58`: no entry point does both caller-supplied
        bars and a `RunProvenance`. `qe_run` loads from the config it
        records, so it publishes one. `run_backtest` is handed a series
        by the caller, so it must not — an artifact naming
        `data/ibkr/.../AAPL.csv` over numbers from a hand-built array is
        a forgery nothing downstream can detect.

        This test fails if someone later "helpfully" attaches
        provenance to the bound result, which is the single most likely
        way this invariant gets broken.
        """
        self.assertIn("provenance", self.cli)
        self.assertIn("data", self.cli["provenance"])

        for attr in ("provenance", "to_json", "run_provenance"):
            self.assertFalse(
                hasattr(self.bound, attr),
                f"PortfolioRun grew `{attr}`. A caller-supplied-bars run must not carry "
                f"provenance — see provenance.hpp:41-58. If this is intended, the "
                f"`caller_supplied` marker and bars hash go in the C++ FIRST.",
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
