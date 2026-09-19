"""PER-241 / S3 — MultiBarSeries <-> numpy.

`BarSeries` is parallel `std::vector<double>` by architectural rule
(CLAUDE.md, "Struct-of-Arrays, always"), which is exactly numpy's
layout. The properties worth testing are therefore not "does it return
numbers" but: is it a VIEW, does the owner stay alive under it, and
does it refuse the input shapes that would break the engine quietly.
"""

import gc
import sys
import unittest

import qe

DAY = 86_400_000_000_000
T0 = 1_600_000_000_000_000_000


def ts(n, start=0):
    return [T0 + (start + i) * DAY for i in range(n)]


def col(n, base=0.0):
    return [base + i for i in range(n)]


def series(n=4, start=0, close=None):
    return qe.BarSeries.from_arrays(
        timestamps=ts(n, start),
        open=col(n),
        high=col(n, 1.0),
        low=col(n, -1.0),
        close=close if close is not None else col(n, 0.5),
        volume=col(n, 100.0),
    )


class Views(unittest.TestCase):
    def test_columns_are_numpy_of_the_right_dtype(self):
        s = series(4)
        self.assertEqual(s.timestamps.dtype.name, "int64")
        for name in ("open", "high", "low", "close", "volume"):
            with self.subTest(column=name):
                self.assertEqual(getattr(s, name).dtype.name, "float64")
                self.assertEqual(getattr(s, name).shape, (4,))

    def test_columns_are_views_not_copies(self):
        # Two accesses must alias the same engine buffer. A fresh copy
        # per access would be correct-looking and would throw away the
        # only reason the engine stores bars this way.
        s = series(4)
        self.assertEqual(
            s.close.__array_interface__["data"][0],
            s.close.__array_interface__["data"][0],
        )
        self.assertFalse(s.close.flags.owndata)

    def test_views_are_read_only(self):
        # The engine's buffers are not Python's to write through, and a
        # per-bar mutation surface is PER-244's hot-loop boundary in a
        # different costume.
        s = series(4)
        self.assertFalse(s.close.flags.writeable)
        with self.assertRaises(ValueError):
            s.close[0] = 1.0

    def test_a_view_keeps_its_owner_alive(self):
        """The keep-alive contract, observed rather than inferred.

        Two earlier versions of this test were VACUOUS and both passed
        with `rv_policy::reference_internal` removed:

          * `sys.getrefcount(self)` rises either way — the returned
            array's `.base` is a memoryview, not the BarSeries, so the
            refcount says nothing about who owns what;
          * reading values back after dropping the owner succeeded
            because freed memory had not been reused yet. That is a
            use-after-free reporting success, which is the exact failure
            mode this property exists to prevent.

        A weakref is decidable: either the view holds the owner or it
        does not. `nb::is_weak_referenceable()` on the class exists for
        this test.

        Caveat, stated so nobody reads more into this than it holds:
        this test also passes with `rv_policy::reference_internal`
        removed. No configuration was found in which the view dangles,
        so this pins the PROPERTY, not the mechanism, and it is not
        evidence that the policy is doing the work.
        """
        import weakref

        s = series(4)
        ref = weakref.ref(s)
        v = s.close
        del s
        gc.collect()
        gc.collect()
        self.assertIsNotNone(ref(), "the view did not keep its owner alive — dangling buffer")
        del v
        gc.collect()
        gc.collect()
        self.assertIsNone(ref(), "the owner outlived its last view — the keep-alive leaks")


class Validation(unittest.TestCase):
    """The engine's own `invariants_ok()` is the gate; these pin what it enforces."""

    def test_decreasing_timestamps_are_refused_and_the_index_is_named(self):
        # The engine fills at bar i+1's open. An unordered grid breaks
        # that silently and nothing downstream can detect it.
        with self.assertRaises(ValueError) as c:
            qe.BarSeries.from_arrays(
                timestamps=[T0 + 2 * DAY, T0 + DAY, T0],
                open=col(3), high=col(3), low=col(3), close=col(3), volume=col(3),
            )
        self.assertIn("decrease at index 1", str(c.exception))

    def test_equal_adjacent_timestamps_are_permitted(self):
        # invariants_ok() checks NON-DECREASING, not strictly
        # increasing. Pinned so the binding never quietly tightens a
        # rule the engine does not have.
        s = qe.BarSeries.from_arrays(
            timestamps=[T0, T0, T0 + DAY],
            open=col(3), high=col(3), low=col(3), close=col(3), volume=col(3),
        )
        self.assertEqual(len(s), 3)

    def test_a_ragged_column_is_refused_and_named(self):
        with self.assertRaises(ValueError) as c:
            qe.BarSeries.from_arrays(
                timestamps=ts(3), open=col(3), high=col(2), low=col(3),
                close=col(3), volume=col(3),
            )
        self.assertIn("'high'", str(c.exception))
        self.assertIn("2 rows", str(c.exception))


class Multi(unittest.TestCase):
    def test_symbol_major_indexing_is_preserved(self):
        a, b = series(3, close=[1.0, 2.0, 3.0]), series(3, close=[9.0, 8.0, 7.0])
        mb = qe.MultiBarSeries.from_series(symbols=["AAA", "BBB"], series=[a, b])
        self.assertEqual(mb.symbols, ["AAA", "BBB"])
        self.assertEqual(mb.symbol_count(), 2)
        self.assertEqual(list(mb.bars[0].close), [1.0, 2.0, 3.0])
        self.assertEqual(list(mb.bars[1].close), [9.0, 8.0, 7.0])

    def test_symbol_index_is_none_when_absent(self):
        mb = qe.MultiBarSeries.from_series(symbols=["AAA"], series=[series(3)])
        self.assertEqual(mb.symbol_index("AAA"), 0)
        self.assertIsNone(mb.symbol_index("ZZZ"))

    def test_the_grid_is_empty_until_aligned(self):
        # Before alignment the per-symbol grids may legitimately differ,
        # and `timestamps` says so by being empty rather than by
        # guessing one of them.
        a = series(3, start=0, close=[1.0, 2.0, 3.0])
        b = series(3, start=1, close=[9.0, 8.0, 7.0])
        mb = qe.MultiBarSeries.from_series(symbols=["AAA", "BBB"], series=[a, b])
        self.assertEqual(list(mb.timestamps), [])
        mb.align_intersection()
        self.assertEqual(list(mb.timestamps), ts(2, 1))
        self.assertEqual(mb.bar_count(), 2)
        # Each symbol was reduced to the shared grid, dropping its own edge.
        self.assertEqual(list(mb.bars[0].close), [2.0, 3.0])
        self.assertEqual(list(mb.bars[1].close), [9.0, 8.0])

    def test_no_common_grid_raises_rather_than_half_aligning(self):
        a = series(2, start=0)
        b = series(2, start=90)
        mb = qe.MultiBarSeries.from_series(symbols=["AAA", "BBB"], series=[a, b])
        with self.assertRaises(ValueError):
            mb.align_intersection()

    def test_symbols_and_series_must_be_the_same_length(self):
        # Symbol-major indexing is the engine's whole addressing scheme;
        # a short zip attaches the right bars to the wrong name.
        with self.assertRaises(ValueError):
            qe.MultiBarSeries.from_series(symbols=["A"], series=[series(3), series(3)])


class ComposesWithAnalytics(unittest.TestCase):
    def test_closes_from_a_multibarseries_feed_ic_analysis(self):
        # S3 and S5 are separate tickets; this is the seam between them,
        # and it is worth one test that the shapes actually meet.
        n = 60
        syms = ["AAA", "BBB", "CCC"]
        mb = qe.MultiBarSeries.from_series(
            symbols=syms,
            series=[series(n, close=[float((i * (k + 2)) % 17) for i in range(n)])
                    for k in range(len(syms))],
        )
        mb.align_intersection()
        factor = [list(mb.bars[i].close) for i in range(mb.symbol_count())]
        rets = [[0.001 * ((i + k) % 5 - 2) for i in range(mb.bar_count())]
                for k in range(mb.symbol_count())]
        r = qe.ic_analysis(factor=factor, returns=rets, horizon=5)
        self.assertEqual(r.n_symbols, 3)
        self.assertGreater(r.n_obs, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
