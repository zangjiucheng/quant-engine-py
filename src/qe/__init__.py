"""quant-engine, from Python.

`Python drives the engine; Python does not participate in it.` There is
no API here that the backtest loop calls back into — see PER-244 for why
that is a correctness property and not a taste.

## The import-time version assert

Two repos, no CI, and a C++ API with no stability guarantee. The failure
this package is insurance against is a binding that was compiled against
one engine and is being read as though it spoke for another: it does not
crash, it returns *subtly different numbers*.

So the extension carries the version and commit it was compiled from,
baked in by CMake from the submodule's own `project()` and `git HEAD`.
At import, if the submodule source is present — which it is for the
editable install this package is normally used as — its current commit
is compared against the compiled-in one and a mismatch **raises**.

A warning would be wrong. The whole point is that the mismatch is
invisible in the results, so the only useful moment to stop is before
anyone has any.
"""

from __future__ import annotations

import pathlib
import subprocess

from qe import _core
from qe._core import IcResult, ic_analysis

__all__ = ["IcResult", "ic_analysis", "engine_version", "engine_commit", "StaleBindingError"]

engine_version = _core.__engine_version__
engine_commit = _core.__engine_commit__
engine_describe = _core.__engine_describe__


class StaleBindingError(RuntimeError):
    """The compiled extension and the engine source on disk disagree."""


def _submodule_head() -> str | None:
    """The submodule's current commit, or None when there is no source tree.

    A wheel installed away from its source has nothing to compare
    against; that is not a failure, it is a different situation, and it
    reports the baked-in version instead of inventing a check.
    """
    root = pathlib.Path(__file__).resolve().parents[2] / "extern" / "quant-engine"
    if not (root / ".git").exists():
        return None
    try:
        out = subprocess.run(
            ["git", "-C", str(root), "rev-parse", "HEAD"],
            capture_output=True,
            text=True,
            timeout=10,
            check=True,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    return out.stdout.strip() or None


def _check() -> None:
    on_disk = _submodule_head()
    if on_disk is None or on_disk == engine_commit:
        return
    raise StaleBindingError(
        f"this extension was compiled against quant-engine {engine_version} "
        f"({engine_commit[:12]}, {engine_describe}), but extern/quant-engine is now at "
        f"{on_disk[:12]}.\n"
        f"The binding still imports and still returns numbers — they are just the OLD "
        f"engine's numbers wearing the new engine's version. Rebuild:\n"
        f"    pip install --no-build-isolation -e .\n"
        f"or put the submodule back:\n"
        f"    git -C extern/quant-engine checkout {engine_commit[:12]}"
    )


_check()
