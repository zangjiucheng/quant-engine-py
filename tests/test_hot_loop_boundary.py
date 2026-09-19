"""PER-244 / S6 — "Python drives, Python does not participate", enforced.

The rule, from PER-222:

    Python may start a run and read its results. Python may NOT be
    called from inside `Engine::run()`.

Not a preference. The engine's hot loop is allocation-free with no
virtual dispatch — the engine's own `tests/test_zero_alloc.cpp` holds
that with a counting allocator — and CLAUDE.md's performance targets
assume it. A per-bar Python callback takes the GIL once per bar and
allocates on every one. The binding that lets someone write a strategy
in Python is the binding that voids every number in that table while
`test_zero_alloc` still passes, because the allocations happen on the
Python side of the boundary where that test is not looking.

A rule that lives only in a ticket gets designed around. This makes it
mechanical.

## Two checks, because either alone can pass for the wrong reason

**Behavioural** — count Python frames entered while the engine runs.
This is the direct analogue of `test_zero_alloc` counting allocations:
it measures the property rather than a proxy for it.

**Structural** — no bound function accepts a callable. A callback has
to arrive through a parameter, so a sweep of every signature in the
module catches one that is added but not yet wired.

## Why there is a positive control

PER-238's lesson: `test_zero_alloc` only caught its bug once the loop
was widened from 360 bars to 4000, because the boundary it needed to
cross was a `std::deque` node size. A guard that passes because it
cannot see anything is worse than no guard.

So `test_the_counter_can_actually_see_a_per_bar_callback` drives the
same bar count through a Python callback and requires the counter to
notice. If that control ever fails, the zero-frames assertion below it
means nothing and the suite says so.

That control is a pure-Python loop, which proves the counting
mechanism works but not that it sees a C++ -> Python crossing
specifically. The stronger check was run ONCE, by hand, while building
this: a temporary `TEMP_run_with_hook` binding that invoked a Python
callable from C++ once per bar produced **exactly 5000 frames for 5000
bars**, and the structural sweep below failed naming its signature.
Both were then removed. Making that control permanent would mean
shipping a callable-taking binding purely to test for
callable-taking bindings, which is adding the hazard in order to
detect it.
"""

import math
import sys
import unittest

import qe

N_BARS = 5000
"""Deliberately large. A per-bar callback over 5000 bars is 5000 Python
frames — three orders of magnitude above any incidental overhead, so
the assertion needs no tolerance and no judgement call. 60 (the smoke
fixture's length) would also detect it, and would leave the reader
wondering whether a handful of frames was noise."""


def synthetic_spy(n=N_BARS):
    ts = [1_577_836_800_000_000_000 + i * 86_400_000_000_000 for i in range(n)]
    px = [100.0 + 10.0 * math.sin(i / 50.0) + 0.01 * i for i in range(n)]
    s = qe.BarSeries.from_arrays(
        timestamps=ts, open=px, high=[p + 1 for p in px], low=[p - 1 for p in px],
        close=px, volume=[1e6] * n,
    )
    mb = qe.MultiBarSeries.from_series(symbols=["SPY"], series=[s])
    mb.align_intersection()
    return mb


class FrameCounter:
    """Counts Python function calls between enter and exit."""

    def __init__(self):
        self.calls = 0

    def _hook(self, frame, event, arg):
        if event == "call":
            self.calls += 1

    def __enter__(self):
        self.calls = 0
        sys.setprofile(self._hook)
        return self

    def __exit__(self, *exc):
        sys.setprofile(None)
        return False


class HotLoopBoundary(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import pathlib

        cls.engine = pathlib.Path(__file__).resolve().parents[1] / "extern" / "quant-engine"
        cls.bars = synthetic_spy()

    def _config(self):
        import contextlib
        import os

        @contextlib.contextmanager
        def chdir(p):
            prev = os.getcwd()
            os.chdir(p)
            try:
                yield
            finally:
                os.chdir(prev)

        with chdir(self.engine):
            return qe.load_qe("tests/fixtures/qe_run_smoke.qe")

    def test_the_counter_can_actually_see_a_per_bar_callback(self):
        """Positive control. Without this the assertion below is unfalsifiable."""

        def per_bar(i):
            return i

        with FrameCounter() as fc:
            for i in range(N_BARS):
                per_bar(i)
        self.assertGreaterEqual(
            fc.calls, N_BARS,
            "the frame counter cannot see a per-bar Python callback, so the "
            "zero-frames assertion in this file proves nothing",
        )

    def test_the_engine_enters_no_python_frames_while_it_runs(self):
        cfg = self._config()
        bars = self.bars
        self.assertEqual(bars.bar_count(), N_BARS)

        with FrameCounter() as fc:
            result = qe.run_backtest(cfg, bars)
        calls = fc.calls

        # The engine really ran: 5000 bars in, 5000 equity points out.
        self.assertEqual(len(result.combined_equity), N_BARS)

        # A per-bar hook would be >= N_BARS. Allow a tiny constant for
        # the call machinery itself; anything near the bar count is the
        # failure this test exists for.
        self.assertLess(
            calls, 100,
            f"{calls} Python frames entered during a {N_BARS}-bar run. Python is being "
            f"called from inside the engine loop — that takes the GIL once per bar, "
            f"allocates on every one, and voids CLAUDE.md's performance targets while "
            f"test_zero_alloc still passes (it does not watch the Python side).",
        )

    def test_no_bound_function_accepts_a_callable(self):
        """Structural sweep: a callback has to arrive through a parameter."""
        suspicious = ("Callable", "collections.abc.Callable", "function", "Coroutine")
        offenders = []

        def check(name, obj):
            doc = getattr(obj, "__doc__", None)
            if not doc:
                return
            sig = doc.splitlines()[0]
            if "(" not in sig:
                return
            params = sig[sig.index("(") + 1 : sig.rfind(")")] if ")" in sig else ""
            for marker in suspicious:
                if marker in params:
                    offenders.append(f"{name}: {sig}")

        for name in dir(qe._core):
            if name.startswith("__"):
                continue
            obj = getattr(qe._core, name)
            check(name, obj)
            if isinstance(obj, type):
                for attr in dir(obj):
                    if attr.startswith("__"):
                        continue
                    check(f"{name}.{attr}", getattr(obj, attr, None))

        self.assertEqual(
            offenders, [],
            "a bound entry point takes a callable. If the engine ever invokes it "
            "per bar, that is the boundary this ticket exists to hold. If it is "
            "genuinely a one-shot callback, add it to this test's allowlist "
            "deliberately and say why.",
        )

    def test_strategies_come_from_the_qe_not_from_python(self):
        # There is no way to express a Python strategy through this
        # surface — `make_expression_strategy` is fed a SignalValue that
        # the .qe evaluator produced. This pins that no strategy
        # construction leaks in later.
        for name in ("IStrategyBase", "make_expression_strategy", "Strategy", "on_bar"):
            self.assertFalse(
                hasattr(qe._core, name),
                f"`{name}` is exposed. Strategies must come from the .qe config; a "
                f"Python-constructible strategy is a per-bar callback wearing a "
                f"different name.",
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
