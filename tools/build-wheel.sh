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
# `-e`, not `-d`. A submodule's `.git` is a FILE -- a 47-byte gitdir
# pointer -- so `-d` is false for a perfectly good checkout and this
# refused to build against a submodule that was right there.
# `qe/__init__.py` had it right next door with `.exists()`.
#
# And a second check, because `.git` alone only says "git knows about
# this": a submodule recorded but never fetched leaves the directory
# empty, and CMake's failure for that is forty missing headers.
[[ -e "$SUB/.git" ]] \
    || fail "the engine submodule is not checked out: git submodule update --init --recursive"
[[ -f "$SUB/CMakeLists.txt" ]] \
    || fail "the engine submodule is registered but empty: git submodule update --init --recursive"

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
echo "▸ triplet    ${TRIPLET_ARGS[0]:-(none — see the warning above)}"

# An interpreter that can actually build. The system python3 here is
# nix's and has no pip at all, which surfaces mid-build as
# "No module named pip" -- after the engine version, the floor and the
# triplet have all been printed as though everything were fine. The
# repo's own .venv is what the README and publish-bindings-pin.sh use.
PY="${PY:-}"
if [[ -z "$PY" ]]; then
    if [[ -x "$HERE/.venv/bin/python" ]]; then PY="$HERE/.venv/bin/python"; else PY="$(command -v python3)"; fi
fi
[[ -n "$PY" ]] || fail "no python3 found"
"$PY" -m pip --version >/dev/null 2>&1 \
    || fail "$PY has no pip, so it cannot build a wheel.
       The system python3 on this machine is nix's and ships without it.
       Use the repo venv ($HERE/.venv) or set PY=/path/to/python."
echo "▸ python     $PY ($("$PY" -c 'import sys;print(sys.version.split()[0])'))"

rm -rf "$HERE/dist"

# SEMICOLONS. `SKBUILD_CMAKE_ARGS` is a ;-separated list, and spaces
# make the whole string ONE argument -- so
# `CMAKE_OSX_DEPLOYMENT_TARGET` took the value
# "13.0 -DCMAKE_OSX_ARCHITECTURES=arm64 -DVCPKG_TARGET_TRIPLET=..."
# and scikit-build-core said so in a warning nobody was reading:
#
#   WARNING - CMAKE_OSX_DEPLOYMENT_TARGET not readable (13.0 -D...)
#
# The triplet silently never applied, vcpkg built against this
# machine's SDK, and the wheel still TAGGED itself macosx_13_0 because
# the tag comes from the environment variable. A correct label on
# wrong contents -- the exact defect the triplet exists to prevent,
# reproduced one layer up.
CMAKE_ARGS="-DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET;-DCMAKE_OSX_ARCHITECTURES=arm64"
for a in "${TRIPLET_ARGS[@]:-}"; do [[ -n "$a" ]] && CMAKE_ARGS="$CMAKE_ARGS;$a"; done
export SKBUILD_CMAKE_ARGS="$CMAKE_ARGS"
BUILD_LOG="$HERE/dist-build.log"

echo "==> building the wheel"
# `--wheel` only. `python -m build` would also produce the sdist that
# step 2 above exists to prevent, and a later `twine upload dist/*`
# would then publish it without anyone deciding to.
"$PY" -m pip install --quiet --upgrade build
mkdir -p "$HERE/dist"
"$PY" -m build --wheel --outdir "$HERE/dist" 2>&1 | tee "$BUILD_LOG"
[[ "${PIPESTATUS[0]}" -eq 0 ]] || fail "the wheel build failed — see $BUILD_LOG"

# The gate `publish-cli.sh` has and this did not, which is how the
# semicolon bug above got as far as printing VERIFIED. A wheel that
# claims macOS $MACOSX_DEPLOYMENT_TARGET while embedding object code
# built against a newer SDK runs here and aborts at dyld on an older
# Mac; the linker is the only thing that sees it, and only as a
# warning.
sdk_warnings=$(grep -c "was built for newer 'macOS' version" "$BUILD_LOG" || true)
[[ "$sdk_warnings" -eq 0 ]] || fail \
    "$sdk_warnings object files were built against a newer SDK than macOS $MACOSX_DEPLOYMENT_TARGET.
       The wheel would still be TAGGED macosx_${MACOSX_DEPLOYMENT_TARGET%%.*}_0_arm64 and still
       abort at dyld on an older Mac. Check that SKBUILD_CMAKE_ARGS is
       semicolon-separated and that the triplet applied. See $BUILD_LOG"
echo "  ✓ no SDK-mismatch warnings"

# And that the triplet was the one asked for, read back from the build
# rather than assumed from the argument.
if [[ ${#TRIPLET_ARGS[@]} -gt 0 ]]; then
    grep -q 'triplet arm64-osx-dist' "$BUILD_LOG" \
        || fail "vcpkg did not use arm64-osx-dist. See $BUILD_LOG"
    echo "  ✓ vcpkg used arm64-osx-dist"
fi

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
"$PY" -m venv "$VENV"
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
