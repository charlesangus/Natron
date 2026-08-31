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
# it re-execs itself through devshell.sh exactly once.
#
# "Already inside the container" detection: devshell.sh's container always
# exports NATRON_IN_CONTAINER=1 (see its `docker run -e NATRON_IN_CONTAINER=1
# ...`), and this script checks that first -- it is an explicit, unambiguous
# signal, not an inference. It also has to work when invoked from inside CI's
# own container (a separate, GitHub-Actions-managed container that runs this
# script directly, without going through devshell.sh at all -- see
# M8.P2.T2), where nothing sets NATRON_IN_CONTAINER. For that case we fall
# back to inferring "already in a container" from CI (checked
# case-insensitively: GitHub Actions sets `CI=true` lowercase, devshell.sh
# sets `CI=True`, and a strict one-spelling compare against just "True"
# would silently mis-detect Actions' lowercase value and try to start Docker
# inside a job that has no Docker daemon).
#
# /.dockerenv was considered as a further fallback (it's written by the
# Docker runtime itself, not by any script here, so in principle it's a more
# fundamental "am I in a container at all" signal than an env var). It was
# rejected: verified empirically that it is NOT reliable here, because the
# machine this repo is developed/CI'd from can itself already be inside an
# unrelated container (e.g. an agent sandbox) that is not natron-dev and
# has no cmake/ninja/ccache -- /.dockerenv being present there caused
# in_container() to falsely report "yes" and run cmake/ninja directly on
# that outer container instead of re-execing into natron-dev, breaking the
# host path. Two explicit-or-CI signals are enough; a third, broader
# "any container" signal is actively wrong for this repo. See
# in_container() below.
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

# Returns success if we're already running inside a dev/CI container (see
# the big comment block above for why each of these checks exists).
in_container() {
    if [[ "${NATRON_IN_CONTAINER:-}" == "1" ]]; then
        return 0
    fi
    local ci_lower
    ci_lower="$(printf '%s' "${CI:-}" | tr '[:upper:]' '[:lower:]')"
    if [[ "${ci_lower}" == "true" ]]; then
        return 0
    fi
    return 1
}

# --- host-side: submodules, then hop into the container ---------------------
if ! in_container; then
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
    # devshell.sh's `docker exec` does not inherit the host shell's
    # environment -- only what the container itself was started with (see
    # devshell.sh's `docker run -e ...` list) is visible inside. Forward
    # NATRON_BUILD_JOBS explicitly via `env` if the caller set it, so e.g.
    # `NATRON_BUILD_JOBS=2 tools/ci/local/build.sh` from the host still
    # reaches the `ninja -j` invocation below, which happens inside the
    # container. `env` with no VAR=val arguments just execs its remaining
    # arguments normally, so this is a no-op when NATRON_BUILD_JOBS is unset.
    ENV_FORWARD=()
    if [[ -n "${NATRON_BUILD_JOBS:-}" ]]; then
        ENV_FORWARD+=("NATRON_BUILD_JOBS=${NATRON_BUILD_JOBS}")
    fi
    exec "${SCRIPT_DIR}/devshell.sh" env "${ENV_FORWARD[@]}" "${REPO_ROOT}/tools/ci/local/build.sh" "$@"
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

# Parallelism: defaults to nproc (the previous hardcoded behaviour), but is
# overridable -- GitHub-hosted runners and this machine don't necessarily
# have the same core count, and CI may want a lower number to bound memory
# use. e.g. NATRON_BUILD_JOBS=2 tools/ci/local/build.sh
JOBS="${NATRON_BUILD_JOBS:-$(nproc)}"

echo "== build.sh: building (${BUILD_TYPE}) with ninja -j${JOBS} =="
echo "+ cd ${BUILD_DIR}"
echo "+ ninja -j${JOBS}"
( cd "${BUILD_DIR}" && ninja -j"${JOBS}" )

echo "== build.sh: build complete: ${BUILD_DIR} =="

if [[ "${SHOW_CCACHE_STATS}" -eq 1 ]]; then
    echo "== build.sh: ccache summary =="
    ccache -s
fi
