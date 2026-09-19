#!/bin/bash
# PER-242 — build `qe_run` FROM THE PINNED SUBMODULE, for parity.
#
# Not from the developer's own quant-engine checkout: that is on `main`
# and the submodule is on a tag, so comparing against it would compare
# two different engines and call any difference a binding bug.
#
# Configures the submodule STANDALONE because this repo's own
# CMakeLists force-disables QE_BUILD_APPS (a Python module has no use
# for the CLIs, and building them for every wheel is minutes wasted).
set -euo pipefail
cd "$(dirname "$0")/.."
: "${VCPKG_ROOT:?export VCPKG_ROOT=\$HOME/vcpkg}"
BUILD=build/qe_run
cmake -S extern/quant-engine -B "$BUILD" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
      -DQE_BUILD_TESTS=OFF -DQE_BUILD_BENCHMARKS=OFF -DQE_BUILD_DASHBOARD=OFF \
      -DVCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON >/dev/null
cmake --build "$BUILD" --target qe_run -j >/dev/null
echo "$(pwd)/$BUILD/bin/qe_run"
