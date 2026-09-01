#!/usr/bin/env bash
#
# Run the same two test steps CI runs against an already-built tree
# (build/debug or build/release, see build.sh): `ctest -V`, and the Python
# bindings smoke test driven through the built NatronRenderer. Both are run
# with the exact environment ci.yml uses (OFX_PLUGIN_PATH pointed at
# the local asset cache from fetch-assets.sh, everything under
# `xvfb-run --auto-servernum` because ci.yml's `defaults.run.shell` wraps
# every step in Xvfb -- see the "Run Unix Tests" / "Python bindings smoke
# test" steps).
#
# Usage:
#   tools/ci/local/test.sh <ctest|smoke> [debug|release] [--gdb]
#
#   ctest                  -> `ctest -V` in the build dir, exactly as ci.yml.
#   smoke                  -> tools/ci/smoke_test.py run through the built
#                              NatronRenderer, located the same way ci.yml
#                              locates it (`find . -maxdepth 4 -type f -name
#                              NatronRenderer | head -n1` from the build dir).
#   debug (default)|release -> selects build/debug or build/release, same
#                              argument style as build.sh.
#   --gdb                   -> run the target (ctest's failing invocation
#                              isn't a single binary, so this applies to
#                              `smoke` only -- see below) under
#                              `gdb -batch -ex run -ex "thread apply all bt"
#                              -ex bt --args <binary> <args>` instead of
#                              running it directly, so a crash yields an
#                              immediate backtrace instead of a bare exit
#                              code. This is the whole point of this script:
#                              it replaces a ci(temp)-diagnostic-commit +
#                              push + wait-for-Actions cycle with something
#                              that runs locally in seconds.
#
# Like build.sh, this is meant to be run from the host, from the repo root,
# with no prior `devshell.sh` shell required -- it re-execs itself through
# devshell.sh exactly once, and does nothing special if already inside the
# container. Detection uses the same in_container() logic as build.sh
# (NATRON_IN_CONTAINER=1, exported by devshell.sh's `docker run`, checked
# first; a case-insensitive CI check as fallback) -- see build.sh's comment
# for the full reasoning, including why a strict CI=="True" compare is not
# reliable under GitHub Actions (CI=true, lowercase) and why /.dockerenv was
# considered and rejected as a further fallback.

set -euo pipefail

# --- argument parsing --------------------------------------------------------
SUBCOMMAND=""
BUILD_TYPE="debug"
USE_GDB=0

for arg in "$@"; do
    case "${arg}" in
        ctest|smoke)
            SUBCOMMAND="${arg}"
            ;;
        debug|release)
            BUILD_TYPE="${arg}"
            ;;
        --gdb)
            USE_GDB=1
            ;;
        *)
            echo "test.sh: unknown argument '${arg}'" >&2
            echo "usage: test.sh <ctest|smoke> [debug|release] [--gdb]" >&2
            exit 1
            ;;
    esac
done

if [[ -z "${SUBCOMMAND}" ]]; then
    echo "test.sh: missing subcommand" >&2
    echo "usage: test.sh <ctest|smoke> [debug|release] [--gdb]" >&2
    exit 1
fi

# Resolve repo root from this script's own location, not $PWD, so this works
# the same from any worktree and regardless of caller cwd.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." >/dev/null 2>&1 && pwd)"

# Returns success if we're already running inside a dev/CI container -- see
# build.sh's identical helper for why each of these checks exists.
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

# --- host-side: hop into the container --------------------------------------
if ! in_container; then
    echo "== test.sh: entering dev container via devshell.sh =="
    exec "${SCRIPT_DIR}/devshell.sh" "${REPO_ROOT}/tools/ci/local/test.sh" "$@"
fi

# --- from here on, we are inside the dev container ---------------------------

case "${BUILD_TYPE}" in
    debug)
        BUILD_DIR="${REPO_ROOT}/build/debug"
        ;;
    release)
        BUILD_DIR="${REPO_ROOT}/build/release"
        ;;
esac

if [[ ! -d "${BUILD_DIR}" ]]; then
    echo "test.sh: ERROR: build dir not found: ${BUILD_DIR}" >&2
    echo "Run tools/ci/local/build.sh ${BUILD_TYPE} first." >&2
    exit 1
fi

ASSETS_DIR="${REPO_ROOT}/build/assets"
OFX_PLUGIN_PATH="${ASSETS_DIR}/Plugins"

if [[ ! -d "${OFX_PLUGIN_PATH}" || -z "$(ls -A "${OFX_PLUGIN_PATH}" 2>/dev/null)" ]]; then
    echo "test.sh: ERROR: build/assets is missing or incomplete." >&2
    echo "Expected a non-empty ${OFX_PLUGIN_PATH}." >&2
    echo "Run tools/ci/local/fetch-assets.sh first." >&2
    exit 1
fi

export OFX_PLUGIN_PATH

# Unset rather than pointed at build/assets/OpenColorIO-Configs, so that what the
# tests run against is the config Natron itself resolves -- the one users get --
# instead of an on-disk config no default install has. smoke_test.py asserts which
# config that turned out to be; an inherited OCIO would silently defeat it.
unset OCIO

case "${SUBCOMMAND}" in
    ctest)
        if [[ "${USE_GDB}" -eq 1 ]]; then
            echo "test.sh: --gdb has no effect on 'ctest' (ctest runs many binaries," >&2
            echo "not a single target) -- ignoring." >&2
        fi
        echo "== test.sh: ctest -V (${BUILD_TYPE}) =="
        echo "+ cd ${BUILD_DIR}"
        echo "+ OFX_PLUGIN_PATH=${OFX_PLUGIN_PATH} xvfb-run --auto-servernum ctest -V"
        cd "${BUILD_DIR}"
        exec xvfb-run --auto-servernum ctest -V
        ;;

    smoke)
        SMOKE_TEST_SCRIPT="${REPO_ROOT}/tools/ci/smoke_test.py"
        if [[ ! -f "${SMOKE_TEST_SCRIPT}" ]]; then
            echo "test.sh: ERROR: ${SMOKE_TEST_SCRIPT} does not exist on this branch." >&2
            echo "tools/ci/smoke_test.py is an M2-era addition that has not been merged" >&2
            echo "to the branch this checkout is on -- there is nothing to run. This is" >&2
            echo "expected on e.g. RB-2.6; 'smoke' is only usable on branches that carry" >&2
            echo "tools/ci/smoke_test.py." >&2
            exit 1
        fi

        cd "${BUILD_DIR}"
        echo "== test.sh: locating NatronRenderer (${BUILD_TYPE}) =="
        echo "+ find . -maxdepth 4 -type f -name NatronRenderer | head -n1"
        NATRON_RENDERER="$(find . -maxdepth 4 -type f -name NatronRenderer | head -n1)"
        if [[ -z "${NATRON_RENDERER}" ]]; then
            echo "test.sh: ERROR: could not find a built NatronRenderer binary under ${BUILD_DIR}" >&2
            echo "Run tools/ci/local/build.sh ${BUILD_TYPE} first." >&2
            exit 1
        fi
        # Resolve to an absolute path so it reads clearly in the echoed
        # command below regardless of the leading "./".
        NATRON_RENDERER="$(cd "$(dirname "${NATRON_RENDERER}")" && pwd)/$(basename "${NATRON_RENDERER}")"
        echo "Using NatronRenderer: ${NATRON_RENDERER}"

        if [[ "${USE_GDB}" -eq 1 ]]; then
            echo "== test.sh: smoke test under gdb (${BUILD_TYPE}) =="
            echo "+ OFX_PLUGIN_PATH=${OFX_PLUGIN_PATH} xvfb-run --auto-servernum \\"
            echo "    gdb -batch -ex run -ex \"thread apply all bt\" -ex bt --args ${NATRON_RENDERER} ${SMOKE_TEST_SCRIPT}"
            exec xvfb-run --auto-servernum \
                gdb -batch -ex run -ex "thread apply all bt" -ex bt \
                --args "${NATRON_RENDERER}" "${SMOKE_TEST_SCRIPT}"
        else
            echo "== test.sh: smoke test (${BUILD_TYPE}) =="
            echo "+ OFX_PLUGIN_PATH=${OFX_PLUGIN_PATH} xvfb-run --auto-servernum \\"
            echo "    ${NATRON_RENDERER} ${SMOKE_TEST_SCRIPT}"
            exec xvfb-run --auto-servernum "${NATRON_RENDERER}" "${SMOKE_TEST_SCRIPT}"
        fi
        ;;
esac
