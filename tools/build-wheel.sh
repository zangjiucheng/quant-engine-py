#!/usr/bin/env bash
# Build the publishable macOS wheel, and prove it works somewhere else.
#
#   tools/build-wheel.sh [--skip-verify]
#
# Built here rather than in CI because the engine is a private
# submodule on SourceHut: a runner would need a key, and SourceHut's
# SSH keys are ACCOUNT-level, so that key would read every private
# repository there. See .github/workflows/publish.yml.
#
# Three things this does that `pip wheel .` does not:
#
#   1. Pins the macOS floor, for the binary AND for vcpkg's ports.
#      Without the second, vcpkg builds OpenSSL and curl against the
#      build machine's SDK while the wheel claims a lower floor -- the
#      linker says so thousands of times and nothing else does. The
#      engine hit this on 2026-10-28 and the fix is its own triplet.
#   2. Refuses to produce a source distribution. An sdist would carry
#      the private engine source to PyPI permanently; a yank does not
#      take it back.
#   3. Installs the wheel into a CLEAN venv outside this tree and
#      imports it. The dev venv has every build dependency already
#      present, so it cannot tell you what a user gets -- this repo
#      has already shipped an undeclared numpy that only a fresh
#      clone revealed.
set -euo pipefail

fail() { echo "error: $*" >&2; exit 1; }

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"

SKIP_VERIFY=0
[[ "${1:-}" == "--skip-verify" ]] && SKIP_VERIFY=1

[[ "$(uname -s)" == "Darwin" ]] || fail "this builds a macOS wheel; run it on macOS"
[[ "$(uname -m)" == "arm64" ]] || fail "this builds an arm64 wheel; run it on Apple Silicon"
[[ -n "${VCPKG_ROOT:-}" ]] || fail "VCPKG_ROOT is not set (the engine uses vcpkg in manifest mode)"

SUB="$HERE/extern/quant-engine"
[[ -d "$SUB/.git" ]] || fail "the engine submodule is not checked out: git submodule update --init --recursive"

# The floor, and the single place it is written. The engine's own
# `dist` preset uses the same number; they must not drift.
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-13.0}"

TRIPLETS="$SUB/cmake/triplets"
TRIPLET_ARGS=()
if [[ -f "$TRIPLETS/arm64-osx-dist.cmake" ]]; then
    TRIPLET_ARGS=(
        "-DVCPKG_TARGET_TRIPLET=arm64-osx-dist"
        "-DVCPKG_OVERLAY_TRIPLETS=$TRIPLETS"
    )
else
    # Loud, not silent. A wheel built without it still builds, still
    # tags itself with the floor below, and still embeds ports built
    # against a newer SDK -- which is the failure this exists to stop.
    cat >&2 <<EOF
warning: the engine pin has no cmake/triplets/arm64-osx-dist.cmake.
         vcpkg will build its ports against this machine's SDK while the
         wheel claims macOS $MACOSX_DEPLOYMENT_TARGET. That combination
         runs here and aborts at dyld on an older Mac.
         Move the submodule to a tag that has the triplet (v0.5.3+)
         before publishing. Continuing for a local build only.
EOF
fi

echo "▸ engine     $(git -C "$SUB" describe --tags --always) ($(git -C "$SUB" rev-parse --short HEAD))"
echo "▸ floor      macOS $MACOSX_DEPLOYMENT_TARGET"
echo "▸ triplet    ${TRIPLET_ARGS[1]:-(none — see the warning above)}"

rm -rf "$HERE/dist"
CMAKE_ARGS="-DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET -DCMAKE_OSX_ARCHITECTURES=arm64"
for a in "${TRIPLET_ARGS[@]:-}"; do [[ -n "$a" ]] && CMAKE_ARGS="$CMAKE_ARGS $a"; done
export SKBUILD_CMAKE_ARGS="$CMAKE_ARGS"

echo "==> building the wheel"
# `--wheel` only. `python -m build` would also produce the sdist that
# step 2 above exists to prevent, and a later `twine upload dist/*`
# would then publish it without anyone deciding to.
python3 -m pip install --quiet --upgrade build
python3 -m build --wheel --outdir "$HERE/dist"

shopt -s nullglob
wheels=("$HERE/dist"/*.whl)
[[ ${#wheels[@]} -eq 1 ]] || fail "expected exactly one wheel, found ${#wheels[@]}"
WHEEL="${wheels[0]}"
sdists=("$HERE/dist"/*.tar.gz)
[[ ${#sdists[@]} -eq 0 ]] || fail "a source distribution appeared: ${sdists[*]} — it must not be published"

echo "==> what was built"
echo "  $(basename "$WHEEL")"
case "$(basename "$WHEEL")" in
    *arm64.whl) ;;
    *) fail "$(basename "$WHEEL") is not an arm64 wheel" ;;
esac

if [[ "$SKIP_VERIFY" -eq 1 ]]; then
    echo "==> verification skipped"
    exit 0
fi

# The part that matters. A venv in /tmp with nothing in it, so the
# only things present are what the wheel declares.
echo "==> verifying in a clean venv outside this tree"
VENV="$(mktemp -d)/venv"
trap 'rm -rf "$(dirname "$VENV")"' EXIT
python3 -m venv "$VENV"
"$VENV/bin/pip" install --quiet "$WHEEL"

# Run from a directory that is NOT this repo, so the import-time
# submodule check takes its wheel path rather than finding this
# source tree and comparing against it.
( cd "$(dirname "$VENV")" && "$VENV/bin/python" - <<'PY'
import qe

print("  import ok")
print("  engine_version =", qe.engine_version)
print("  engine_commit  =", qe.engine_commit[:12])
missing = [n for n in ("run_backtest", "load_qe", "load_bars", "sharpe",
                       "ic_analysis", "BarSeries", "MultiBarSeries") if not hasattr(qe, n)]
assert not missing, f"the wheel is missing {missing}"
# numpy is a runtime dependency because every bar column comes back as
# an ndarray; this is the line that catches it going undeclared again.
import numpy as np
s = qe.BarSeries.from_arrays(
    timestamps=np.array([1, 2, 3], dtype="int64"),
    open=np.array([1.0, 2.0, 3.0]), high=np.array([1.0, 2.0, 3.0]),
    low=np.array([1.0, 2.0, 3.0]), close=np.array([1.0, 2.0, 3.0]),
    volume=np.array([1.0, 1.0, 1.0]))
assert len(s.close) == 3, "a bar column did not come back"
print("  a bar column came back:", list(s.close))
print("  VERIFIED")
PY
)

echo
echo "wheel: $WHEEL"
echo "next:  gh release create v<version> '$WHEEL' --repo zangjiucheng/quant-engine-py"
