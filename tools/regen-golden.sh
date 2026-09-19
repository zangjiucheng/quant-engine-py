#!/bin/bash
# PER-239 — regenerate tests/ic_golden.json from the ENGINE.
#
# A moved golden is a claim that the engine's numbers changed. Run this
# deliberately, read the diff, and say why in the commit.
set -euo pipefail
cd "$(dirname "$0")/.."
: "${VCPKG_ROOT:?export VCPKG_ROOT=\$HOME/vcpkg — it is not set in non-interactive shells}"
BUILD=build/golden
PY="$(pwd)/.venv/bin/python"
# scikit-build injects nanobind_DIR during a wheel build; a standalone
# configure has to supply it. The generator does not link nanobind at
# all, but CMakeLists find_package()s it unconditionally for the module.
NB_DIR="$("$PY" -c 'import nanobind; print(nanobind.cmake_dir())')"
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
      -DPython_EXECUTABLE="$PY" -Dnanobind_DIR="$NB_DIR" >/dev/null
cmake --build "$BUILD" --target gen_ic_golden -j >/dev/null
"./$BUILD/gen_ic_golden" tests/ic_golden.json
