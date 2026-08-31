#!/usr/bin/env bash
#
# Configure (once) and incrementally build Natron with CMake + Ninja +
# ccache, inside the long-lived dev container from devshell.sh. The whole
# point is that a re-run after a small edit finishes in seconds instead of
# tens of minutes, because configure is skipped once the CMake cache exists
# and ccache serves most of the compiler invocations from the previous run.
#
# Usage:
#   tools/ci/local/build.sh [debug|release] [--reconfigure] [--no-ccache-stats]
#
#   debug (default)      -> build/debug,   configured with -DCMAKE_BUILD_TYPE=Debug
#   release               -> build/release, configured with NO explicit build
#                            type, matching ci.yml's release job exactly.
#   --reconfigure         -> force re-running `cmake` even if the build
#                            directory already has a CMakeCache.txt (use this
#                            after e.g. editing CMakeLists.txt).
#   --no-ccache-stats      -> skip the `ccache -s` summary printed at the end.
#
# This script runs the actual configure/build INSIDE the dev container (that
# is where cmake/ninja/ccache/gcc/Qt6 live), but is meant to be invoked from
# the host, from the repo root, with no prior `devshell.sh` shell required --
# it re-execs itself through devshell.sh exactly once. devshell.sh's
# container always exports CI=True (see its `docker run -e CI=True ...`),
# and nothing on the host sets that, so "CI=True is already set" is a
# reliable "we're already inside the dev container" check -- it prevents
# nesting a second `docker exec` inside the first when this script is run
# from an interactive `devshell.sh` shell.
#
# Submodule initialization deliberately happens on the HOST side, before the
# re-exec into the container: this repo's submodules (Tests/google-mock,
# Tests/google-test, libs/OpenFX, libs/SequenceParsing) may be uninitialized,
# and running `git submodule` from inside the container -- where the
# invoking uid owns files freshly, but the container's git config/HOME differ
# from the host's -- risks ownership/config complications. Doing it on the
# host, where the checkout's git config already lives, avoids all of that.

set -euo pipefail

# --- argument parsing --------------------------------------------------------
BUILD_TYPE="debug"
RECONFIGURE=0
SHOW_CCACHE_STATS=1

for arg in "$@"; do
    case "${arg}" in
        debug|release)
            BUILD_TYPE="${arg}"
            ;;
        --reconfigure)
            RECONFIGURE=1
            ;;
        --no-ccache-stats)
            SHOW_CCACHE_STATS=0
            ;;
        *)
            echo "build.sh: unknown argument '${arg}'" >&2
            echo "usage: build.sh [debug|release] [--reconfigure] [--no-ccache-stats]" >&2
            exit 1
            ;;
    esac
done

# Resolve repo root from this script's own location, not $PWD, so this works
# the same from any worktree and regardless of caller cwd.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." >/dev/null 2>&1 && pwd)"

# --- host-side: submodules, then hop into the container ---------------------
if [[ "${CI:-}" != "True" ]]; then
    echo "== build.sh: ensuring submodules are initialized (host side) =="
    if ! git -C "${REPO_ROOT}" submodule update --init --recursive; then
        echo "build.sh: ERROR: 'git submodule update --init --recursive' failed." >&2
        echo "This is required for the build (Tests/google-mock, Tests/google-test," >&2
        echo "libs/OpenFX, libs/SequenceParsing must be checked out). Resolve the" >&2
        echo "submodule fetch error above (network access to github.com is required)" >&2
        echo "and re-run this script." >&2
        exit 1
    fi

    echo "== build.sh: entering dev container via devshell.sh =="
    exec "${SCRIPT_DIR}/devshell.sh" "${REPO_ROOT}/tools/ci/local/build.sh" "$@"
fi

# --- from here on, we are inside the dev container ---------------------------

case "${BUILD_TYPE}" in
    debug)
        BUILD_DIR="${REPO_ROOT}/build/debug"
        # Mirrors ci.yml's "Build Unix (debug)" step.
        CMAKE_BUILD_TYPE_ARGS=(-DCMAKE_BUILD_TYPE=Debug)
        ;;
    release)
        BUILD_DIR="${REPO_ROOT}/build/release"
        # Mirrors ci.yml's "Build Unix (release)" step, which passes no
        # explicit -DCMAKE_BUILD_TYPE at all -- match that exactly, don't
        # "fix" it to Release.
        CMAKE_BUILD_TYPE_ARGS=()
        ;;
esac

mkdir -p "${BUILD_DIR}"

CMAKE_CACHE="${BUILD_DIR}/CMakeCache.txt"

CMAKE_ARGS=(
    -G Ninja  # Intentional local/CI difference: ci.yml uses `make`, we use
              # Ninja for build speed. This is the one deliberate divergence
              # in the build itself -- everything else here matches CI.
    "${CMAKE_BUILD_TYPE_ARGS[@]}"
    -DCMAKE_PREFIX_PATH=/usr/local
    -DCMAKE_C_COMPILER_LAUNCHER=ccache
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    "${REPO_ROOT}"
)

if [[ -f "${CMAKE_CACHE}" && "${RECONFIGURE}" -eq 0 ]]; then
    echo "== build.sh: ${BUILD_DIR}/CMakeCache.txt already exists -- skipping configure =="
    echo "   (pass --reconfigure to force re-running cmake)"
else
    echo "== build.sh: configuring (${BUILD_TYPE}) =="
    echo "+ cd ${BUILD_DIR}"
    echo "+ cmake ${CMAKE_ARGS[*]}"
    ( cd "${BUILD_DIR}" && cmake "${CMAKE_ARGS[@]}" )
fi

echo "== build.sh: building (${BUILD_TYPE}) with ninja -j$(nproc) =="
echo "+ cd ${BUILD_DIR}"
echo "+ ninja -j$(nproc)"
( cd "${BUILD_DIR}" && ninja -j"$(nproc)" )

echo "== build.sh: build complete: ${BUILD_DIR} =="

if [[ "${SHOW_CCACHE_STATS}" -eq 1 ]]; then
    echo "== build.sh: ccache summary =="
    ccache -s
fi
