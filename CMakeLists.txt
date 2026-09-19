cmake_minimum_required(VERSION 3.25)

# The engine's own manifest defaults to gui + tests + benchmarks. The
# bindings link none of them, and building GLFW / ImGui / ImPlot /
# Catch2 / Google Benchmark to produce a Python module is minutes of
# compile for nothing. Set BEFORE project(), which is when vcpkg's
# toolchain reads the manifest.
set(VCPKG_MANIFEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/quant-engine" CACHE PATH "")
set(VCPKG_MANIFEST_NO_DEFAULT_FEATURES ON CACHE BOOL "")

# vcpkg's toolchain, composed from VCPKG_ROOT rather than assumed to be
# passed. VCPKG_ROOT is NOT set in non-interactive shells on the build
# machine — `publish-release.sh` and a bare `cmake --preset` have both
# been bitten by that — so read it here and fail with a sentence rather
# than with forty missing headers.
if(NOT CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE FILEPATH "")
    else()
        message(FATAL_ERROR
            "VCPKG_ROOT is not set and no CMAKE_TOOLCHAIN_FILE was given. The engine "
            "uses vcpkg in manifest mode; without the toolchain this fails later and "
            "less clearly. Export VCPKG_ROOT=$HOME/vcpkg (it is not set in "
            "non-interactive shells) or pass -DCMAKE_TOOLCHAIN_FILE.")
    endif()
endif()

project(quant-engine-py LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---- the engine, from the submodule, at a tag ------------------------
#
# Source, not a prebuilt. The only published artifact is `dist`-preset:
# Apple-Silicon-only with a pinned deployment target that
# `publish-release.sh` gates on, which is not what a Python module
# wants. Compiling the pinned tag is slower and is the only way to be
# certain the binding and the engine are the same code.
set(QE_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
set(QE_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(QE_BUILD_APPS       OFF CACHE BOOL "" FORCE)
set(QE_BUILD_DASHBOARD  OFF CACHE BOOL "" FORCE)
# SHIM, deletable once the submodule pin moves past the fix.
#
# quant-engine v0.4.2's CMakeLists appends "${CMAKE_SOURCE_DIR}/cmake"
# to CMAKE_MODULE_PATH — the TOP-LEVEL source dir, which under
# `add_subdirectory` is this project, not the engine. Its
# `include(QeCodesign)` then resolved against the wrong tree and the
# configure died on `Unknown CMake command "qe_codesign_setup"`. Every
# other path in that file already used CMAKE_CURRENT_SOURCE_DIR; this
# one line was the exception, and being consumed as a subdirectory is
# what surfaced it.
#
# Fixed upstream on main. The pin is a TAG (v0.4.2), by design, so the
# fix is not here yet and will arrive with v0.4.3 — at which point this
# block goes away. Prepending costs nothing while it is redundant.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/extern/quant-engine/cmake")

add_subdirectory(extern/quant-engine EXCLUDE_FROM_ALL)

# ---- what version did we actually build against? ---------------------
#
# Baked in at configure time and asserted at import (see qe/__init__.py).
# `quant-engine_VERSION` comes from the submodule's own project() call,
# so it cannot drift from the engine's idea of its version.
execute_process(
    COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}/extern/quant-engine" rev-parse HEAD
    OUTPUT_VARIABLE QE_ENGINE_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE QE_GIT_RC)
if(NOT QE_GIT_RC EQUAL 0)
    message(FATAL_ERROR
        "cannot read the engine submodule's commit. The version assert is the "
        "point of this package; building without it is not a degraded mode, it "
        "is the failure it exists to prevent.")
endif()
execute_process(
    COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}/extern/quant-engine" describe --tags --always
    OUTPUT_VARIABLE QE_ENGINE_DESCRIBE OUTPUT_STRIP_TRAILING_WHITESPACE)
message(STATUS "engine: ${quant-engine_VERSION} @ ${QE_ENGINE_DESCRIBE} (${QE_ENGINE_COMMIT})")

# ---- the module ------------------------------------------------------
find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module)
find_package(nanobind CONFIG REQUIRED)

nanobind_add_module(_core STABLE_ABI NB_STATIC src/qe_bindings.cpp)
target_link_libraries(_core PRIVATE qe::qe)
# The VERSION is not passed: the binding reads `qe::KVersion` from the
# engine's own generated header, which is the single source of truth its
# docstring insists on. `project()`'s `<name>_VERSION` is directory-scoped
# anyway and came through EMPTY here, which is how that was noticed.
# The commit and describe are git facts about the submodule, which only
# this file knows.
target_compile_definitions(_core PRIVATE
    QE_ENGINE_COMMIT="${QE_ENGINE_COMMIT}"
    QE_ENGINE_DESCRIBE="${QE_ENGINE_DESCRIBE}")

install(TARGETS _core LIBRARY DESTINATION qe)

# ---- the parity golden generator -------------------------------------
#
# EXCLUDE_FROM_ALL: a dev tool, never part of the wheel. Built and run
# by tools/regen-golden.sh when the reference needs regenerating, which
# should be rare and deliberate — a moved golden is a claim that the
# ENGINE changed, the same posture `scripts/pin-run-baselines.py` takes
# in the engine repo.
add_executable(gen_ic_golden EXCLUDE_FROM_ALL tools/gen_ic_golden.cpp)
target_link_libraries(gen_ic_golden PRIVATE qe::qe)
