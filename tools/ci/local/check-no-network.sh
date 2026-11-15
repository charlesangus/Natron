#!/usr/bin/env bash
#
# Runs a command for a while with every outbound network call refused and logged, then fails if
# it tried to reach anything off the machine.
#
# Usage:
#   tools/ci/local/check-no-network.sh [--seconds N] [--xvfb] [--setting KEY=VALUE]...
#                                      [--expect-ort] [--expect-exe NAME] [--keep] -- <command> [args...]
#
#   --seconds N        run the command for N seconds (default 30, at least 1), then SIGTERM it;
#                      a command that exits sooner is fine.
#   --xvfb             start a private Xvfb and run the command on it with the natron-dev
#                      container's software-GL recipe (as in build/deeprepro/run-gui.sh).
#   --setting K=V      add K=V to [General] in the fresh Natron.conf, for example
#                      --setting aiEnabled=true. checkForUpdates=false is always written, since a
#                      first run otherwise stops on a modal question.
#   --expect-ort       exit 2 unless check-no-network-load-ort.py reported a loaded ONNX Runtime
#                      (the AI Detected Providers line starts with "ONNX Runtime"), so an AI
#                      run cannot pass without the library ever loading. The report is printed
#                      whenever the script wrote one.
#   --expect-exe NAME  exit 2 unless a process named NAME (the executable's short name, for
#                      example Natron or NatronRenderer) loaded the shim. Without it, any
#                      program other than a shell, env or timeout will do: a launcher script
#                      loads the shim in its shell without ever starting Natron.
#   --keep             keep the work directory (fresh HOME, shim, log) and print its path.
#
# The command runs with a fresh HOME and XDG directories, and LD_PRELOAD set to a shim built
# from check-no-network-shim.c beside this script. The shim refuses, and logs, every name
# lookup other than localhost, numeric addresses and the machine's own name, every
# connect/sendto/sendmsg to an AF_INET or AF_INET6 address other than loopback, and anything
# to port 53. AF_UNIX (X11, D-Bus) is allowed. It works where strace is missing and
# `unshare -n` is denied, but cannot see raw syscalls or statically linked programs.
#
# CI, GITHUB_ACTIONS and the other variables ONNX Runtime reads as "running on a build server"
# are removed from the command's environment, as is ORT_DISABLE_TELEMETRY: with any of them set,
# ONNX Runtime never starts its telemetry client, and the natron-dev container sets CI=True. The
# program under test has to switch telemetry off itself.
#
# aiEnabled=true alone loads nothing: ONNX Runtime is loaded when the Preferences window opens or
# an AI node renders. Pass check-no-network-load-ort.py, beside this script, as the script to
# run; it makes Natron load ONNX Runtime the way Preferences does, and in NatronRenderer -t it
# then waits 30 s before exiting, since the telemetry client's first lookup comes about 8 s
# later, so give it 60 s. The GUI keeps running until it is stopped.
#
# --xvfb needs /tmp/.X11-unix to be writable, so in the natron-dev container run it as root
# (docker exec -u root) after `chmod 1777 /tmp/.X11-unix`.
#
# Examples, from the natron-dev container as root with LD_LIBRARY_PATH unset:
#   tools/ci/local/check-no-network.sh --seconds 60 --setting aiEnabled=true --expect-ort \
#       --expect-exe NatronRenderer -- \
#       squashfs-root/bin/NatronRenderer -t tools/ci/local/check-no-network-load-ort.py
#   tools/ci/local/check-no-network.sh --seconds 60 --xvfb --setting aiEnabled=true --expect-ort \
#       --expect-exe Natron -- \
#       squashfs-root/AppRun tools/ci/local/check-no-network-load-ort.py
#
# Exit status: 0 nothing tried to leave the machine; 1 something did (the calls are printed);
# 2 usage or setup errors, the shim never loaded, or the command failed before its time was up.

set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SHIM_SOURCE="${HERE}/check-no-network-shim.c"

RUN_SECONDS=30
USE_XVFB=0
KEEP=0
EXPECT_ORT=0
EXPECT_EXE=""
SETTINGS=()

die() {
    echo "check-no-network: $*" >&2
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --seconds)
            [ $# -ge 2 ] || die "--seconds needs a value"
            RUN_SECONDS="$2"
            shift 2
            ;;
        --xvfb)
            USE_XVFB=1
            shift
            ;;
        --setting)
            [ $# -ge 2 ] || die "--setting needs KEY=VALUE"
            case "$2" in *=*) ;; *) die "--setting needs KEY=VALUE, not '$2'" ;; esac
            SETTINGS+=("$2")
            shift 2
            ;;
        --expect-ort)
            EXPECT_ORT=1
            shift
            ;;
        --expect-exe)
            [ $# -ge 2 ] || die "--expect-exe needs a name"
            EXPECT_EXE="$2"
            shift 2
            ;;
        --keep)
            KEEP=1
            shift
            ;;
        --)
            shift
            break
            ;;
        -h|--help)
            sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            die "unknown option '$1' (the command goes after --)"
            ;;
    esac
done
[ $# -gt 0 ] || die "no command given (put it after --)"
case "${RUN_SECONDS}" in
    ''|*[!0-9]*) die "--seconds takes a whole number, not '${RUN_SECONDS}'" ;;
esac
[ "${RUN_SECONDS}" -ge 1 ] || die "--seconds must be at least 1: timeout treats 0 as no limit"
[ -f "${SHIM_SOURCE}" ] || die "missing ${SHIM_SOURCE}"

WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/natron-no-network.XXXXXX")
XVFB_PID=""
cleanup() {
    if [ -n "${XVFB_PID}" ]; then
        kill "${XVFB_PID}" 2>/dev/null || true
        wait "${XVFB_PID}" 2>/dev/null || true
    fi
    if [ "${KEEP}" = 1 ]; then
        echo "check-no-network: work directory kept at ${WORK_DIR}" >&2
    else
        rm -rf "${WORK_DIR:?}"
    fi
}
trap cleanup EXIT

SHIM="${WORK_DIR}/no-network.so"
LOG="${WORK_DIR}/network.log"
OUTPUT="${WORK_DIR}/output.log"
"${CC:-cc}" -shared -fPIC -O2 -Wall -o "${SHIM}" "${SHIM_SOURCE}" -ldl \
    || die "could not build the shim with ${CC:-cc}"

TEST_HOME="${WORK_DIR}/home"
mkdir -p "${TEST_HOME}/.config/INRIA" "${TEST_HOME}/.cache" "${TEST_HOME}/.local/share"
{
    echo "[General]"
    echo "checkForUpdates=false"
    for setting in "${SETTINGS[@]+"${SETTINGS[@]}"}"; do
        echo "${setting}"
    done
} > "${TEST_HOME}/.config/INRIA/Natron.conf"

UNSET_ENV=()
for name in CI TF_BUILD GITHUB_ACTIONS GITLAB_CI CIRCLECI TRAVIS JENKINS_URL CODEBUILD_BUILD_ID \
    BUILDKITE TEAMCITY_VERSION APPVEYOR BITBUCKET_BUILD_NUMBER SYSTEM_TEAMFOUNDATIONCOLLECTIONURI \
    ORT_RUNNING_UNIT_TESTS ORT_DISABLE_TELEMETRY; do
    UNSET_ENV+=(-u "${name}")
done

RUN_ENV=(
    "HOME=${TEST_HOME}"
    "XDG_CONFIG_HOME=${TEST_HOME}/.config"
    "XDG_CACHE_HOME=${TEST_HOME}/.cache"
    "XDG_DATA_HOME=${TEST_HOME}/.local/share"
    "NATRON_NO_NETWORK_LOG=${LOG}"
    "LD_PRELOAD=${SHIM}${LD_PRELOAD:+:${LD_PRELOAD}}"
)

if [ "${USE_XVFB}" = 1 ]; then
    command -v Xvfb >/dev/null || die "--xvfb needs Xvfb"
    if [ -e /tmp/.X11-unix ] && [ ! -w /tmp/.X11-unix ]; then
        die "--xvfb needs a writable /tmp/.X11-unix: run as root after chmod 1777 /tmp/.X11-unix"
    fi
    # The container's /etc/ld.so.preload forces a libGL without a mesa vendor onto everything,
    # Xvfb included; the system copies in /usr/lib64 are the ones that can do GLX.
    GL_DIR="${WORK_DIR}/gl"
    mkdir -p "${GL_DIR}"
    for lib in /usr/lib64/libGLX.so.0 /usr/lib64/libGL.so.1 /usr/lib64/libOpenGL.so.0 \
        /usr/lib64/libGLdispatch.so.0 /usr/lib64/libEGL.so.1 /usr/lib64/libGLX_mesa.so.0 \
        /usr/lib64/libLLVM.so.*; do
        if [ -e "${lib}" ]; then
            ln -sf "${lib}" "${GL_DIR}/"
        fi
    done
    DISPLAY_FILE="${WORK_DIR}/display"
    LD_LIBRARY_PATH=/usr/lib64 Xvfb -displayfd 3 -screen 0 1920x1080x24 +extension GLX \
        3>"${DISPLAY_FILE}" >"${WORK_DIR}/xvfb.log" 2>&1 &
    XVFB_PID=$!
    for _ in $(seq 1 50); do
        [ -s "${DISPLAY_FILE}" ] && break
        kill -0 "${XVFB_PID}" 2>/dev/null || die "Xvfb exited: $(cat "${WORK_DIR}/xvfb.log")"
        sleep 0.2
    done
    [ -s "${DISPLAY_FILE}" ] || die "Xvfb did not report a display"
    RUN_ENV+=(
        "DISPLAY=:$(tr -d '[:space:]' < "${DISPLAY_FILE}")"
        "LIBGL_ALWAYS_SOFTWARE=1"
        "__GLX_VENDOR_LIBRARY_NAME=mesa"
        "LD_LIBRARY_PATH=${GL_DIR}:/usr/local/lib:/usr/local/lib64${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
    )
fi

echo "check-no-network: running for up to ${RUN_SECONDS} s: $*" >&2
status=0
timeout --signal=TERM --kill-after=15 "${RUN_SECONDS}" env "${UNSET_ENV[@]}" "${RUN_ENV[@]}" "$@" \
    >"${OUTPUT}" 2>&1 || status=$?

touch "${LOG}"
# A launcher script (AppRun) loads the shim in its own shell, which proves nothing about the program.
if [ -n "${EXPECT_EXE}" ]; then
    LOADED_PATTERN=" exe=${EXPECT_EXE} loaded\$"
else
    LOADED_PATTERN=" exe=(?!(sh|bash|dash|env|timeout|busybox) )[^ ]+ loaded\$"
fi
if ! grep -qP -- "${LOADED_PATTERN}" "${LOG}"; then
    echo "--- command output ---" >&2
    tail -n 40 "${OUTPUT}" >&2
    die "the shim never loaded in ${EXPECT_EXE:-a program other than a shell} (exit status ${status}): the command cleared LD_PRELOAD or is static, so nothing was checked"
fi

ORT_REPORT="${TEST_HOME}/load-ort.txt"
if [ -f "${ORT_REPORT}" ]; then
    echo "check-no-network: load-ort report: $(cat "${ORT_REPORT}")" >&2
fi

if grep -q ' block ' "${LOG}"; then
    echo "check-no-network: FAIL: the command tried to reach the network:" >&2
    grep ' block ' "${LOG}" | sort | uniq -c >&2
    exit 1
fi

if [ "${EXPECT_ORT}" = 1 ]; then
    if ! grep -q 'providers=ONNX Runtime' "${ORT_REPORT}" 2>/dev/null; then
        echo "--- command output ---" >&2
        tail -n 40 "${OUTPUT}" >&2
        die "ONNX Runtime was never loaded (no load-ort.txt, or its providers line does not start with \"ONNX Runtime\"), so the check proves nothing about it"
    fi
fi

# 124: still running when the time was up, which is the normal case for an interactive program;
# 137: it ignored SIGTERM and was killed.
if [ "${status}" != 0 ] && [ "${status}" != 124 ] && [ "${status}" != 137 ]; then
    echo "--- command output ---" >&2
    tail -n 40 "${OUTPUT}" >&2
    die "the command exited with status ${status} before its time was up, so the check proves little"
fi

echo "check-no-network: OK: no outbound lookups or connections ($(grep -c ' allow ' "${LOG}" || true) local lookups allowed)" >&2
