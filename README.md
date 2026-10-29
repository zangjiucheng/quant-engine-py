# quant-engine-py

Python bindings for **quant-engine** — a C++20 backtesting engine with
struct-of-arrays bar data, a zero-allocation hot loop, and a parallel
parameter sweep harness.

**Python drives the engine; Python does not participate in it.** You
start runs and read results. There is no per-bar callback and there
will not be one: it would take the GIL ~2500 times per backtest and
allocate on every one, voiding the performance properties the engine
exists for. That is a correctness boundary, enforced by a test that
counts Python frames entered during a run — not a roadmap item.

## Install

```bash
pip install quant-engine-py
```

**macOS on Apple Silicon, Python 3.10+.** There is no Linux wheel, no
Intel wheel and no source distribution: the engine is a private
repository, so it ships compiled or not at all. `pip install` anywhere
else reports that no matching distribution exists, which is the honest
answer rather than a build that fails halfway.

## Use

```python
import qe

cfg  = qe.load_qe("strategy.qe")   # evaluate a .qe config
bars = qe.load_bars(cfg)           # resolve its data specs
res  = qe.run_backtest(cfg, bars)  # .combined_equity, .per_strategy

qe.engine_version                  # the engine this was compiled against
```

Also bound: `ic_analysis`, `rolling_ic_analysis`, `long_short_backtest`,
and the metrics (`sharpe`, `sortino`, `cagr`, `max_drawdown`,
`total_return`, `kahan_sum`). Bar columns come back as **read-only
numpy views** over the engine's own buffers, not copies.

`qe.engine_version` is not cosmetic. A binding compiled against one
engine and read as though it spoke for another does not crash — it
returns subtly different numbers. Built from a source tree, the module
compares the submodule's commit at import and **raises** on a mismatch.

## Documentation

Everything else — the `.qe` language, factor research, walk-forward
validation, the full bound surface, the sign conventions you need
before reading a drawdown — is at:

**<https://qe.jiucheng-zang.ca/python/>**

The engine's own documentation is at **<https://qe.jiucheng-zang.ca/>**.

## License

All rights reserved. Published for use, not for redistribution.
