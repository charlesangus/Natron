#!/usr/bin/env bash
#
# Run the same two test steps CI runs against an already-built tree
# (build/debug, build/release or build/fast, see build.sh): `ctest -V`, and the Python
# bindings smoke test driven through the built NatronRenderer. Both are run
# with the exact environment ci.yml uses (OFX_PLUGIN_PATH pointed at
# the local asset cache from fetch-assets.sh, everything under
# `xvfb-run --auto-servernum` because ci.yml's `defaults.run.shell` wraps
# every step in Xvfb -- see the "Run Unix Tests" / "Python bindings smoke
# test" steps).
#
# Usage:
#   [NATRON_RENDER_SCHEDULER=legacy|taskgraph] tools/ci/local/test.sh <ctest|smoke|startup> [debug|release|fast] [--gdb]
#   tools/ci/local/test.sh repeat <ctest-name-or-regex> <N> [--parallel P] [--gdb] [debug|release|fast]
#   [NATRON_CTEST_JOBS=<n>] tools/ci/local/test.sh ctest ...
#   [GTEST_SHUFFLE=1] [GTEST_RANDOM_SEED=<n>] [GTEST_REPEAT=<n>] tools/ci/local/test.sh ctest ...
#   [NATRON_SMOKE_OUTPUT_DIR=<dir>|NATRON_SMOKE_REFERENCE_DIR=<dir>] tools/ci/local/test.sh smoke ...
#
#   NATRON_RENDER_SCHEDULER -> selects the render scheduler for ctest and the
#                              smoke test; unset leaves Natron's default.
#                              Forwarded into the container when set.
#   NATRON_CTEST_JOBS       -> ctest only: how many cases run at once, all as
#                              clients of the one Xvfb server. Unset or 1 is
#                              the serial `ctest -V` run. Above 1, `ctest -V`
#                              would interleave the cases' output line by line,
#                              so the run prints only failing cases' output as
#                              each finishes, and the complete output of every
#                              case is in Testing/Temporary/LastTest.log
#                              (printed after the run, folded, under GitHub
#                              Actions). Cases that assert a wall-clock bound
#                              run alone: see Tests/SuiteProperties.cmake.
#                              When the parallel run fails, the failed cases
#                              are re-run serially with `ctest --rerun-failed
#                              -V` so their complete output is in the log; the
#                              exit status stays the parallel run's.
#                              Forwarded into the container when set.
#   GTEST_SHUFFLE,
#   GTEST_RANDOM_SEED,
#   GTEST_REPEAT            -> ctest only: gtest reads these itself, and ctest
#                              hands its environment to every suite process, so
#                              they shuffle the cases within each suite (seed
#                              n, or a time-based one when unset), or repeat
#                              each suite, to flush out leaks between cases.
#                              Each ctest entry is one suite, so the shuffle
#                              never crosses suites. Forwarded into the
#                              container when set.
#   NATRON_SMOKE_OUTPUT_DIR -> smoke only: the graded PNG is copied here.
#   NATRON_SMOKE_REFERENCE_DIR -> smoke only: the graded PNG must be
#                              byte-identical to the same-named file there
#                              (a previous run's NATRON_SMOKE_OUTPUT_DIR).
#                              Both smoke dirs must be paths the container
#                              sees, i.e. under the repo.
#
#   ctest                  -> `ctest -V` (or `ctest -j`, see NATRON_CTEST_JOBS)
#                              in the build dir, exactly as ci.yml.
#   smoke                  -> tools/ci/smoke_test.py run through the built
#                              NatronRenderer, located the same way ci.yml
#                              locates it (`find . -maxdepth 4 -type f -name
#                              NatronRenderer | head -n1` from the build dir).
#   repeat <name> <N>      -> runs one ctest case N times, P instances at a time
#                              (--parallel P, default 4), while `ctest -j 4 -E <name>`
#                              runs the rest of the suite in the background as load.
#                              <name> is a regex that must match exactly one ctest
#                              test; the matched name and command line are printed.
#                              The command comes from `ctest --show-only=json-v1`
#                              and runs with that test's ENVIRONMENT and
#                              WORKING_DIRECTORY. Stops at the first failing
#                              iteration, prints its number and complete output
#                              (the SIGSEGV handler's instruction pointer is enough
#                              for addr2line) and exits non-zero. Every iteration's
#                              output stays in <build dir>/Testing/repeat/<name>/.
#                              An iteration still running 20 s before the test's
#                              TIMEOUT gets its thread stacks written to iter-N.stacks
#                              (eu-stack, else gdb -p; "stacks unavailable" when ptrace
#                              is refused) and its process group killed.
#                              With --gdb each run is wrapped in `gdb -batch -ex
#                              run -ex "thread apply all bt" --args`, and a run
#                              that ends in a signal or a non-zero exit counts as
#                              failed even though gdb itself exits 0.
#   startup                -> starts the built GUI binary under a GLX-capable
#                              Xvfb on software GL and requires its main window
#                              to be exposed (Tests/gui/debug_startup.py, through
#                              Tests/gui/run-gui-test.sh). Meant for `debug`,
#                              which arms the floating-point traps that Mesa's
#                              llvmpipe trips over unless the GL init path
#                              disarms them; no ctest case ever starts a GUI.
#   debug (default)|release|fast -> selects build/debug, build/release or build/fast, same
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
#   The smoke step wraps NatronRenderer in `timeout`: it's the only bare
#   `exec` left with no bound on how long it can run, and a hang there (the
#   scheduler bug this whole exercise guards against is real, not
#   hypothetical -- see OutputSchedulerThread.cpp) would otherwise wedge
#   the CI job until GitHub's six-hour default job timeout kills it, with
#   no diagnostic. The smoke test's own real runtime is ~8s end to end
#   (measured locally, debug build); SMOKE_TIMEOUT_SECONDS below gives it
#   ~35x headroom, matching the 300s per-case bound Tests/CMakeLists.txt
#   puts on ctest.
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

# See the header comment for how this number was chosen.
SMOKE_TIMEOUT_SECONDS=300

# --- argument parsing --------------------------------------------------------
SUBCOMMAND=""
BUILD_TYPE="debug"
USE_GDB=0
REPEAT_NAME=""
REPEAT_COUNT=""
REPEAT_PARALLEL=4
ORIG_ARGS=("$@")
USAGE="usage: test.sh <ctest|smoke|startup> [debug|release|fast] [--gdb]
       test.sh repeat <ctest-name-or-regex> <N> [--parallel P] [--gdb] [debug|release|fast]"

while [[ $# -gt 0 ]]; do
    arg="$1"
    shift
    case "${arg}" in
        ctest|smoke|startup|repeat)
            SUBCOMMAND="${arg}"
            ;;
        debug|release|fast)
            BUILD_TYPE="${arg}"
            ;;
        --gdb)
            USE_GDB=1
            ;;
        --parallel)
            if [[ $# -eq 0 || ! "$1" =~ ^[1-9][0-9]*$ ]]; then
                echo "test.sh: --parallel needs a positive integer, got '${1:-}'" >&2
                exit 1
            fi
            REPEAT_PARALLEL="$1"
            shift
            ;;
        *)
            if [[ "${SUBCOMMAND}" == "repeat" && -z "${REPEAT_NAME}" ]]; then
                REPEAT_NAME="${arg}"
            elif [[ "${SUBCOMMAND}" == "repeat" && -z "${REPEAT_COUNT}" && "${arg}" =~ ^[1-9][0-9]*$ ]]; then
                REPEAT_COUNT="${arg}"
            else
                echo "test.sh: unknown argument '${arg}'" >&2
                echo "${USAGE}" >&2
                exit 1
            fi
            ;;
    esac
done

if [[ -z "${SUBCOMMAND}" ]]; then
    echo "test.sh: missing subcommand" >&2
    echo "${USAGE}" >&2
    exit 1
fi

if [[ "${SUBCOMMAND}" == "repeat" && ( -z "${REPEAT_NAME}" || -z "${REPEAT_COUNT}" ) ]]; then
    echo "test.sh: repeat needs a test name and a positive iteration count" >&2
    echo "${USAGE}" >&2
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
    # `docker exec` does not carry the caller's environment into the
    # long-lived container, so variables that steer the run are re-applied
    # through env(1) on the container side.
    forwarded_env=()
    for var in NATRON_RENDER_SCHEDULER NATRON_CTEST_JOBS GTEST_SHUFFLE GTEST_RANDOM_SEED GTEST_REPEAT NATRON_SMOKE_OUTPUT_DIR NATRON_SMOKE_REFERENCE_DIR; do
        if [[ -n "${!var:-}" ]]; then
            forwarded_env+=("${var}=${!var}")
        fi
    done
    exec "${SCRIPT_DIR}/devshell.sh" env ${forwarded_env[@]+"${forwarded_env[@]}"} \
        "${REPO_ROOT}/tools/ci/local/test.sh" "${ORIG_ARGS[@]}"
fi

# --- from here on, we are inside the dev container ---------------------------

case "${BUILD_TYPE}" in
    debug)
        BUILD_DIR="${REPO_ROOT}/build/debug"
        ;;
    release)
        BUILD_DIR="${REPO_ROOT}/build/release"
        ;;
    fast)
        BUILD_DIR="${REPO_ROOT}/build/fast"
        ;;
esac

if [[ ! -d "${BUILD_DIR}" ]]; then
    echo "test.sh: ERROR: build dir not found: ${BUILD_DIR}" >&2
    echo "Run tools/ci/local/build.sh ${BUILD_TYPE} first." >&2
    exit 1
fi

if [[ ( "${SUBCOMMAND}" == "ctest" || "${SUBCOMMAND}" == "repeat" ) ]] && grep -qs '^NATRON_BUILD_TESTS:[A-Z]*=OFF$' "${BUILD_DIR}/CMakeCache.txt"; then
    echo "test.sh: ERROR: ${BUILD_DIR} was configured without tests." >&2
    echo "Run tools/ci/local/build.sh ${BUILD_TYPE} --tests first." >&2
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

# Plug-ins built at refs other than the ones fetch-assets.sh pins would test something else.
"${SCRIPT_DIR}/fetch-assets.sh" --check-pins || exit 1

export OFX_PLUGIN_PATH

# Unset, so that what the tests run against is the config Natron itself
# resolves -- the one users get -- instead of an on-disk config no default
# install has. smoke_test.py asserts which
# config that turned out to be; an inherited OCIO would silently defeat it.
unset OCIO

case "${SUBCOMMAND}" in
    ctest)
        if [[ "${USE_GDB}" -eq 1 ]]; then
            echo "test.sh: --gdb has no effect on 'ctest' (ctest runs many binaries," >&2
            echo "not a single target) -- ignoring." >&2
        fi
        CTEST_JOBS="${NATRON_CTEST_JOBS:-1}"
        if [[ ! "${CTEST_JOBS}" =~ ^[1-9][0-9]*$ ]]; then
            echo "test.sh: ERROR: NATRON_CTEST_JOBS must be a positive integer, got '${CTEST_JOBS}'" >&2
            exit 1
        fi
        # Only a default: every discovered gtest suite carries its own TIMEOUT property, which
        # wins; this bounds the tests registered without one.
        CTEST_TIMEOUT_SECONDS=300
        cd "${BUILD_DIR}"
        echo "+ cd ${BUILD_DIR}"
        if [[ "${CTEST_JOBS}" -eq 1 ]]; then
            echo "== test.sh: ctest -V (${BUILD_TYPE}) =="
            echo "+ OFX_PLUGIN_PATH=${OFX_PLUGIN_PATH} xvfb-run --auto-servernum ctest -V --timeout ${CTEST_TIMEOUT_SECONDS}"
            exec xvfb-run --auto-servernum ctest -V --timeout "${CTEST_TIMEOUT_SECONDS}"
        fi

        echo "== test.sh: ctest -j ${CTEST_JOBS} (${BUILD_TYPE}) =="
        echo "+ OFX_PLUGIN_PATH=${OFX_PLUGIN_PATH} xvfb-run --auto-servernum ctest -j ${CTEST_JOBS} --timeout ${CTEST_TIMEOUT_SECONDS} --output-on-failure"
        FAILED_LIST="${BUILD_DIR}/Testing/Temporary/LastTestsFailed.log"
        # ctest only rewrites this list when it gets as far as running cases, so
        # a leftover from an earlier run must not trigger a re-run.
        rm -f "${FAILED_LIST}"
        CTEST_STDOUT="${BUILD_DIR}/Testing/Temporary/ctest-stdout.log"
        mkdir -p "$(dirname "${CTEST_STDOUT}")"
        # The capture feeds ctest-summary.sh; rc must stay ctest's own status, not tee's.
        set +e
        xvfb-run --auto-servernum ctest -j "${CTEST_JOBS}" --timeout "${CTEST_TIMEOUT_SECONDS}" --output-on-failure 2>&1 | tee "${CTEST_STDOUT}"
        pipe_status=("${PIPESTATUS[@]}")
        rc=${pipe_status[0]}
        tee_rc=${pipe_status[1]:-0}
        set -e
        if [[ "${tee_rc}" -ne 0 ]]; then
            echo "test.sh: warning: could not write ${CTEST_STDOUT}" >&2
        fi
        FULL_LOG="${BUILD_DIR}/Testing/Temporary/LastTest.log"
        # A failing suite's ctest line names only the suite; the summary adds the gtest
        # cases and the tests that died before reporting any.
        if [[ "${rc}" -ne 0 && -f "${CTEST_STDOUT}" ]]; then
            "${SCRIPT_DIR}/ctest-summary.sh" "${CTEST_STDOUT}"
        fi
        if [[ -f "${FULL_LOG}" ]]; then
            echo "Complete output of every case: ${FULL_LOG}"
            if [[ "${GITHUB_ACTIONS:-}" == "true" ]]; then
                echo "::group::Complete ctest output (LastTest.log)"
                cat "${FULL_LOG}"
                echo "::endgroup::"
            fi
        fi
        if [[ "${rc}" -ne 0 && -s "${FAILED_LIST}" ]]; then
            echo "== test.sh: re-running failed cases serially, ctest --rerun-failed -V (${BUILD_TYPE}) =="
            echo "+ OFX_PLUGIN_PATH=${OFX_PLUGIN_PATH} xvfb-run --auto-servernum ctest --rerun-failed -V --timeout ${CTEST_TIMEOUT_SECONDS}"
            # The re-run is diagnostic only; its status must not replace rc.
            xvfb-run --auto-servernum ctest --rerun-failed -V --timeout "${CTEST_TIMEOUT_SECONDS}" || true
        fi
        exit "${rc}"
        ;;

    repeat)
        # One Xvfb server for the runs and the load, started once: re-enter under it.
        if [[ -z "${NATRON_REPEAT_IN_XVFB:-}" ]]; then
            exec xvfb-run --auto-servernum env NATRON_REPEAT_IN_XVFB=1 "${REPO_ROOT}/tools/ci/local/test.sh" "${ORIG_ARGS[@]}"
        fi
        cd "${BUILD_DIR}"
        SELECTION="$(mktemp)"
        # Emits shell assignments for the one matching test, so the command line survives
        # quoting intact.
        python3 -I - "${REPEAT_NAME}" > "${SELECTION}" <<'PY'
import json, re, shlex, subprocess, sys

pattern = sys.argv[1]
tests = json.loads(subprocess.check_output(["ctest", "--show-only=json-v1"]))["tests"]
hits = [t for t in tests if re.search(pattern, t["name"])]
if len(hits) != 1:
    print("test.sh: '%s' matched %d ctest tests (need exactly 1): %s"
          % (pattern, len(hits), ", ".join(t["name"] for t in hits) or "none"), file=sys.stderr)
    sys.exit(2)
test = hits[0]
props = {p["name"]: p["value"] for p in test.get("properties", [])}
print("REPEAT_TEST=%s" % shlex.quote(test["name"]))
# ctest -E takes a CMake regex, whose metacharacters differ from Python's.
print("REPEAT_EXCLUDE=%s" % shlex.quote("^" + re.sub(r"([][\\^$.|?*+()])", r"\\\1", test["name"]) + "$"))
print("REPEAT_CMD=(%s)" % " ".join(shlex.quote(a) for a in test["command"]))
print("REPEAT_ENV=(%s)" % " ".join(shlex.quote(e) for e in props.get("ENVIRONMENT", [])))
print("REPEAT_CWD=%s" % shlex.quote(props.get("WORKING_DIRECTORY", ".")))
print("REPEAT_TIMEOUT=%d" % int(props.get("TIMEOUT", 300)))
PY
        # shellcheck disable=SC1090
        source "${SELECTION}"
        rm -f "${SELECTION}"
        echo "== test.sh: repeat ${REPEAT_TEST} x ${REPEAT_COUNT}, ${REPEAT_PARALLEL} at a time (${BUILD_TYPE}) =="
        echo "+ cd ${REPEAT_CWD} && env ${REPEAT_ENV[*]} ${REPEAT_CMD[*]}"

        LOG_DIR="${BUILD_DIR}/Testing/repeat/${REPEAT_TEST//[^A-Za-z0-9_.-]/_}"
        mkdir -p "${LOG_DIR}"
        find "${LOG_DIR}" -maxdepth 1 -name 'iter-*' -delete

        # Load and iterations each get their own session (setsid), so killing the process group
        # takes timeout, the test binary and gdb with it; timeout would otherwise escape into a
        # process group of its own.
        LOAD_PID=""
        LOAD_STARTED=0
        LOAD_RESTARTS=1
        start_load() {
            setsid ctest -j 4 -E "${REPEAT_EXCLUDE}" --timeout 300 >> "${LOG_DIR}/load.log" 2>&1 < /dev/null &
            LOAD_PID=$!
            LOAD_STARTED=${SECONDS}
        }
        : > "${LOG_DIR}/load.log"
        start_load

        ITER_PIDS=()
        stop_iterations() {
            local n pgid
            for ((n = 1; n <= ${#ITER_PIDS[@]}; n++)); do
                if [[ -s "${LOG_DIR}/iter-${n}.pgid" ]]; then
                    pgid="$(<"${LOG_DIR}/iter-${n}.pgid")"
                    kill -TERM -- "-${pgid}" 2>/dev/null || true
                fi
                kill -TERM "${ITER_PIDS[n - 1]}" 2>/dev/null || true
            done
            for pgid in "${ITER_PIDS[@]}"; do
                wait "${pgid}" 2>/dev/null || true
            done
        }
        # shellcheck disable=SC2317,SC2329
        stop_all() {
            stop_iterations
            if [[ -n "${LOAD_PID}" ]]; then
                kill -TERM -- "-${LOAD_PID}" 2>/dev/null || true
                wait "${LOAD_PID}" 2>/dev/null || true
            fi
        }
        trap stop_all EXIT

        # The watchdog fires WATCHDOG_LEAD seconds before ctest's own TIMEOUT so the stacks are
        # taken from a hung process that is still alive.
        WATCHDOG_LEAD=20
        WATCHDOG_SECONDS=$((REPEAT_TIMEOUT > WATCHDOG_LEAD ? REPEAT_TIMEOUT - WATCHDOG_LEAD : 1))

        # Writes every thread's stack of $1 to $2. Returns non-zero when ptrace is refused or no
        # stack tool is installed.
        dump_stacks() {
            local pid="$1" out="$2" tmp
            tmp="$(mktemp)"
            if command -v eu-stack > /dev/null 2>&1 && eu-stack -p "${pid}" > "${tmp}" 2>&1 && [[ -s "${tmp}" ]]; then
                cat "${tmp}" > "${out}"
            elif command -v gdb > /dev/null 2>&1 && gdb -p "${pid}" -batch -ex 'thread apply all bt' > "${tmp}" 2>&1 && grep -q '^Thread ' "${tmp}"; then
                cat "${tmp}" > "${out}"
            else
                {
                    echo "stacks unavailable: ptrace refused (yama ptrace_scope or missing CAP_SYS_PTRACE) or no eu-stack/gdb installed"
                    cat "${tmp}"
                } > "${out}"
                rm -f "${tmp}"
                return 1
            fi
            rm -f "${tmp}"
        }

        run_iteration() {
            local n="$1" log="${LOG_DIR}/iter-$1.log" rc child timer
            set +e
            if [[ "${USE_GDB}" -eq 1 ]]; then
                # SIGINT stops the debuggee, so gdb goes on to print every thread's backtrace.
                (cd "${REPEAT_CWD}" && exec setsid timeout -s INT -k 10 "${WATCHDOG_SECONDS}" env ${REPEAT_ENV[@]+"${REPEAT_ENV[@]}"} \
                    gdb -batch -ex run -ex "thread apply all bt" --args "${REPEAT_CMD[@]}") > "${log}" 2>&1 < /dev/null &
                child=$!
                echo "${child}" > "${LOG_DIR}/iter-${n}.pgid"
                wait "${child}"
                rc=$?
                # gdb exits 0 after the debuggee crashed or failed.
                if [[ "${rc}" -eq 0 ]] && grep -qE 'Program (received signal|terminated with signal)|exited with code' "${log}"; then
                    rc=1
                fi
                if [[ "${rc}" -eq 124 ]]; then
                    echo "stacks: see the backtrace at the end of ${log}" > "${LOG_DIR}/iter-${n}.stacks"
                fi
            else
                (cd "${REPEAT_CWD}" && exec setsid env ${REPEAT_ENV[@]+"${REPEAT_ENV[@]}"} \
                    "${REPEAT_CMD[@]}") > "${log}" 2>&1 < /dev/null &
                child=$!
                echo "${child}" > "${LOG_DIR}/iter-${n}.pgid"
                sleep "${WATCHDOG_SECONDS}" &
                timer=$!
                wait -n "${child}" "${timer}"
                rc=$?
                if kill -0 "${timer}" 2>/dev/null; then
                    kill "${timer}" 2>/dev/null
                    wait "${timer}" 2>/dev/null
                else
                    dump_stacks "${child}" "${LOG_DIR}/iter-${n}.stacks"
                    kill -TERM -- "-${child}" 2>/dev/null
                    sleep 10 &
                    timer=$!
                    wait -n "${child}" "${timer}"
                    kill -KILL -- "-${child}" 2>/dev/null
                    kill "${timer}" 2>/dev/null
                    wait "${child}"
                    rc=124
                fi
            fi
            echo "${rc}" > "${LOG_DIR}/iter-${n}.rc"
        }

        live_iterations() {
            local running pid count=0
            running=" $(jobs -rp | tr '\n' ' ') "
            for pid in "${ITER_PIDS[@]}"; do
                if [[ "${running}" == *" ${pid} "* ]]; then
                    count=$((count + 1))
                fi
            done
            echo "${count}"
        }

        # Without the load the iterations no longer run under contention, so keep it going; a
        # load that exits almost at once would only spin, so that one is left down.
        check_load() {
            [[ -n "${LOAD_PID}" ]] || return 0
            if [[ " $(jobs -rp | tr '\n' ' ') " == *" ${LOAD_PID} "* ]]; then
                return 0
            fi
            wait "${LOAD_PID}" 2>/dev/null || true
            local ran=$((SECONDS - LOAD_STARTED))
            if [[ "${ran}" -lt 10 ]]; then
                echo "test.sh: warning: the ctest load exited after ${ran}s with iterations still running; continuing without it (see ${LOG_DIR}/load.log)" >&2
                LOAD_PID=""
                return 0
            fi
            echo "test.sh: warning: the ctest load finished before the iterations; restarting it (run $((LOAD_RESTARTS + 1)))" >&2
            LOAD_RESTARTS=$((LOAD_RESTARTS + 1))
            start_load
        }

        FAILED_ITER=""
        find_failure() {
            local f rc n
            for f in "${LOG_DIR}"/iter-*.rc; do
                [[ -e "${f}" ]] || continue
                rc="$(<"${f}")"
                if [[ "${rc}" -ne 0 ]]; then
                    n="${f##*/iter-}"
                    FAILED_ITER="${n%.rc}"
                    FAILED_RC="${rc}"
                    return 0
                fi
            done
            return 1
        }

        i=1
        while [[ "${i}" -le "${REPEAT_COUNT}" ]]; do
            while [[ "$(live_iterations)" -ge "${REPEAT_PARALLEL}" ]]; do
                wait -n || true
                check_load
            done
            if find_failure; then
                break
            fi
            run_iteration "${i}" &
            ITER_PIDS+=("$!")
            i=$((i + 1))
        done
        while [[ -z "${FAILED_ITER}" && "$(live_iterations)" -gt 0 ]]; do
            wait -n || true
            check_load
            find_failure || true
        done
        if [[ -n "${FAILED_ITER}" ]]; then
            stop_iterations
        fi
        for pid in "${ITER_PIDS[@]}"; do
            wait "${pid}" 2>/dev/null || true
        done
        find_failure || true

        if [[ -n "${FAILED_ITER}" ]]; then
            echo "test.sh: ERROR: ${REPEAT_TEST} failed in iteration ${FAILED_ITER} of ${REPEAT_COUNT} (exit ${FAILED_RC})" >&2
            if [[ -s "${LOG_DIR}/iter-${FAILED_ITER}.stacks" ]]; then
                echo "test.sh: thread stacks at the hang: ${LOG_DIR}/iter-${FAILED_ITER}.stacks" >&2
            fi
            echo "== iteration ${FAILED_ITER} output (${LOG_DIR}/iter-${FAILED_ITER}.log) =="
            cat "${LOG_DIR}/iter-${FAILED_ITER}.log"
            exit 1
        fi
        passed=0
        for ((n = 1; n <= REPEAT_COUNT; n++)); do
            if [[ -s "${LOG_DIR}/iter-${n}.rc" && "$(<"${LOG_DIR}/iter-${n}.rc")" == "0" ]]; then
                passed=$((passed + 1))
            fi
        done
        if [[ "${passed}" -ne "${REPEAT_COUNT}" ]]; then
            echo "test.sh: ERROR: only ${passed} of ${REPEAT_COUNT} iterations of ${REPEAT_TEST} recorded a pass" >&2
            exit 1
        fi
        echo "== test.sh: ${REPEAT_TEST} passed ${REPEAT_COUNT} of ${REPEAT_COUNT} iterations =="
        exit 0
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

        # Not `exec`: a bare exec would replace this shell with the child,
        # leaving nothing around to inspect `timeout`'s exit status and
        # tell a real timeout (124, or 137 if it had to escalate to KILL --
        # see `man timeout`) apart from the smoke test's own failure exit
        # codes. `-k 10` gives a SIGTERM-ignoring hang a follow-up SIGKILL
        # 10s later instead of leaving it around after `timeout` gives up
        # on it. Note gdb's `-ex run -ex "thread apply all bt" -ex bt`
        # sequence only fires once `run` returns, i.e. on a crash or normal
        # exit; a genuine hang never gets there, so --gdb still gets no
        # backtrace for a hang (only for a crash) -- the timeout below is
        # what turns that case into a killed, reported process instead of
        # one that runs until GitHub's six-hour job timeout.
        if [[ "${USE_GDB}" -eq 1 ]]; then
            echo "== test.sh: smoke test under gdb (${BUILD_TYPE}, ${SMOKE_TIMEOUT_SECONDS}s timeout) =="
            echo "+ OFX_PLUGIN_PATH=${OFX_PLUGIN_PATH} xvfb-run --auto-servernum \\"
            echo "    timeout -k 10 ${SMOKE_TIMEOUT_SECONDS} gdb -batch -ex run -ex \"thread apply all bt\" -ex bt --args ${NATRON_RENDERER} ${SMOKE_TEST_SCRIPT}"
            if xvfb-run --auto-servernum \
                timeout -k 10 "${SMOKE_TIMEOUT_SECONDS}" \
                gdb -batch -ex run -ex "thread apply all bt" -ex bt \
                --args "${NATRON_RENDERER}" "${SMOKE_TEST_SCRIPT}"; then
                rc=0
            else
                rc=$?
            fi
        else
            echo "== test.sh: smoke test (${BUILD_TYPE}, ${SMOKE_TIMEOUT_SECONDS}s timeout) =="
            echo "+ OFX_PLUGIN_PATH=${OFX_PLUGIN_PATH} xvfb-run --auto-servernum \\"
            echo "    timeout -k 10 ${SMOKE_TIMEOUT_SECONDS} ${NATRON_RENDERER} ${SMOKE_TEST_SCRIPT}"
            if xvfb-run --auto-servernum \
                timeout -k 10 "${SMOKE_TIMEOUT_SECONDS}" \
                "${NATRON_RENDERER}" "${SMOKE_TEST_SCRIPT}"; then
                rc=0
            else
                rc=$?
            fi
        fi

        if [[ ${rc} -eq 124 || ${rc} -eq 137 ]]; then
            echo "test.sh: ERROR: smoke test exceeded ${SMOKE_TIMEOUT_SECONDS}s and was killed by timeout(1) (exit ${rc})" >&2
        fi
        exit "${rc}"
        ;;

    startup)
        if [[ "${USE_GDB}" -eq 1 ]]; then
            echo "test.sh: --gdb has no effect on 'startup' -- ignoring." >&2
        fi
        NATRON_GUI="${BUILD_DIR}/App/Natron"
        if [[ ! -x "${NATRON_GUI}" ]]; then
            echo "test.sh: ERROR: ${NATRON_GUI} is missing or not executable." >&2
            echo "Run tools/ci/local/build.sh ${BUILD_TYPE} first." >&2
            exit 1
        fi
        echo "== test.sh: GUI startup under Xvfb (${BUILD_TYPE}) =="
        echo "+ Tests/gui/run-gui-test.sh Tests/gui/debug_startup.py ${NATRON_GUI}"
        exec "${REPO_ROOT}/Tests/gui/run-gui-test.sh" "${REPO_ROOT}/Tests/gui/debug_startup.py" "${NATRON_GUI}"
        ;;
esac
