#!/usr/bin/env bash
#
# Run a Natron build on GitHub Actions through the dispatch-only
# remote-build.yml workflow, wait for it, and download the results into
# build/remote/<preset>/. The remote twin of tools/ci/local/build.sh.
#
# Usage:
#   tools/ci/remote/build.sh <fast|release|debug> [--tests] [--record-goldens] [--shuffle-seeds <n,n,...>] [--no-appimage]
#   tools/ci/remote/build.sh --mode
#
#   fast | release | debug -> the build preset, same meaning as in local/build.sh.
#                        debug never produces an AppImage: one cannot be packaged
#                        in the container, because llvmpipe raises SIGFPE under
#                        the -DDEBUG traps. It behaves as --no-appimage.
#   --tests           -> also run the ctest suite and smoke tests remotely; the
#                        ctest logs are downloaded even when the run fails.
#   --record-goldens  -> implies --tests; also records the native golden renders
#                        remotely and downloads them, STAMP included, into
#                        Tests/fixtures/native-golden/ of this checkout. The
#                        references bind bit for bit only under the preset that
#                        recorded them (see STAMP).
#   --shuffle-seeds <n,n,...> -> implies --tests; each ctest leg also runs once
#                        per seed with GTEST_SHUFFLE=1 GTEST_RANDOM_SEED=<seed>,
#                        after its unshuffled run, e.g. --shuffle-seeds 1,2,3.
#                        Every seed runs even if an earlier one failed, and each
#                        run's log is ctest-<scheduler>-seed<seed>.log next to
#                        the unshuffled ctest-<scheduler>.log. The ctest step's
#                        timeout is a fixed 120 minutes, which fits about five
#                        seeds on the debug preset, so at most 5 seeds are
#                        accepted.
#   --no-appimage     -> skip packaging and downloading the AppImage.
#   --mode            -> print the build mode (local or remote) and exit. Takes
#                        no other arguments. The mode is the first non-blank,
#                        non-# line of .local/build-mode at the repo root; a
#                        missing file means remote.
#
# The workflow runs the commit that is pushed, not the working tree, so this
# refuses unless HEAD equals origin/<current branch>. It never pushes.

set -euo pipefail

WORKFLOW="remote-build.yml"
FIND_TIMEOUT_SECS=300
FIND_INTERVAL_SECS=5
WATCH_INTERVAL_SECS=30

usage() {
    echo "usage: build.sh <fast|release|debug> [--tests] [--record-goldens] [--shuffle-seeds <n,n,...>] [--no-appimage]" >&2
    echo "       build.sh --mode" >&2
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
REPO_ROOT="$(git -C "${SCRIPT_DIR}" rev-parse --show-toplevel)"

print_mode() {
    local file="${REPO_ROOT}/.local/build-mode"
    local line value=""
    if [[ -f "${file}" ]]; then
        while IFS= read -r line || [[ -n "${line}" ]]; do
            line="${line#"${line%%[![:space:]]*}"}"
            line="${line%"${line##*[![:space:]]}"}"
            if [[ -z "${line}" || "${line}" == \#* ]]; then
                continue
            fi
            value="${line}"
            break
        done < "${file}"
    fi
    case "${value}" in
        ""|remote) echo "remote" ;;
        local) echo "local" ;;
        *)
            echo "build.sh: ${file}: invalid build mode '${value}' (expected 'local' or 'remote')" >&2
            exit 1
            ;;
    esac
}

PRESET=""
RUN_TESTS=false
RECORD_GOLDENS=false
WANT_APPIMAGE=true
SHUFFLE_SEEDS=""

if [[ " $* " == *" --mode "* ]]; then
    if [[ $# -ne 1 ]]; then
        echo "build.sh: --mode takes no other arguments" >&2
        usage
        exit 1
    fi
    print_mode
    exit 0
fi

while [[ $# -gt 0 ]]; do
    arg="$1"
    shift
    case "${arg}" in
        fast|release|debug)
            if [[ -n "${PRESET}" ]]; then
                echo "build.sh: preset given twice ('${PRESET}' and '${arg}')" >&2
                usage
                exit 1
            fi
            PRESET="${arg}"
            ;;
        --tests)
            RUN_TESTS=true
            ;;
        --record-goldens)
            RECORD_GOLDENS=true
            RUN_TESTS=true
            ;;
        --shuffle-seeds)
            if [[ $# -eq 0 ]]; then
                echo "build.sh: --shuffle-seeds needs a comma-separated list of seeds" >&2
                usage
                exit 1
            fi
            SHUFFLE_SEEDS="$1"
            shift
            RUN_TESTS=true
            ;;
        --no-appimage)
            WANT_APPIMAGE=false
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "build.sh: unknown argument '${arg}'" >&2
            usage
            exit 1
            ;;
    esac
done

if [[ -z "${PRESET}" ]]; then
    echo "build.sh: a preset (fast|release|debug) is required" >&2
    usage
    exit 1
fi

# The ctest step's 120-minute timeout covers about five seeds on the debug preset.
if [[ -n "${SHUFFLE_SEEDS}" ]] && (( $(tr -cd ',' <<<"${SHUFFLE_SEEDS}" | wc -c) + 1 > 5 )); then
    echo "build.sh: --shuffle-seeds accepts at most 5 seeds (the ctest step's 120-minute timeout covers about five), got '${SHUFFLE_SEEDS}'" >&2
    usage
    exit 1
fi

# gtest takes a seed from 0 to 99999; 0 would pick a time-based one, which a rerun cannot repeat.
if [[ -n "${SHUFFLE_SEEDS}" ]] && ! [[ "${SHUFFLE_SEEDS}" =~ ^[1-9][0-9]{0,4}(,[1-9][0-9]{0,4})*$ ]]; then
    echo "build.sh: --shuffle-seeds must be comma-separated integers from 1 to 99999, got '${SHUFFLE_SEEDS}'" >&2
    usage
    exit 1
fi

if [[ "${PRESET}" == "debug" && "${WANT_APPIMAGE}" == "true" ]]; then
    echo "build.sh: the debug preset builds no AppImage (llvmpipe raises SIGFPE under -DDEBUG traps in the container); behaving as --no-appimage" >&2
    WANT_APPIMAGE=false
fi

cd "${REPO_ROOT}"

BRANCH="$(git symbolic-ref --quiet --short HEAD || true)"
if [[ -z "${BRANCH}" ]]; then
    echo "build.sh: HEAD is detached; check out a branch that is pushed to origin" >&2
    exit 1
fi

if ! git fetch --quiet origin "${BRANCH}"; then
    echo "build.sh: could not fetch origin/${BRANCH}; has the branch been pushed?" >&2
    exit 1
fi

HEAD_SHA="$(git rev-parse HEAD)"
REMOTE_SHA="$(git rev-parse --verify --quiet "refs/remotes/origin/${BRANCH}^{commit}" || true)"
if [[ "${HEAD_SHA}" != "${REMOTE_SHA}" ]]; then
    echo "build.sh: HEAD (${HEAD_SHA:0:10}) is not origin/${BRANCH} (${REMOTE_SHA:0:10})." >&2
    echo "The remote build runs the pushed commit, so push first; this script never pushes." >&2
    exit 1
fi

if [[ -n "$(git status --porcelain --untracked-files=normal)" ]]; then
    echo "build.sh: warning: uncommitted changes are not part of the remote build" >&2
fi

TOKEN="${PRESET}-${HEAD_SHA:0:8}-$(od -An -N4 -tx1 /dev/urandom | tr -d ' \n')"

echo "== build.sh: dispatching ${WORKFLOW} on ${BRANCH} (${HEAD_SHA:0:10}) token=${TOKEN} =="
gh workflow run "${WORKFLOW}" --ref "${BRANCH}" \
    -f "preset=${PRESET}" \
    -f "tests=${RUN_TESTS}" \
    -f "record_goldens=${RECORD_GOLDENS}" \
    -f "appimage=${WANT_APPIMAGE}" \
    -f "shuffle_seeds=${SHUFFLE_SEEDS}" \
    -f "token=${TOKEN}"

# gh 2.23 prints no run ID on dispatch, so the run is found by the token that
# the workflow echoes into its run name.
RUN_ID=""
deadline=$((SECONDS + FIND_TIMEOUT_SECS))
while [[ -z "${RUN_ID}" ]]; do
    RUN_ID="$(gh run list --workflow "${WORKFLOW}" --branch "${BRANCH}" \
        --json databaseId,displayTitle,headSha --limit 20 \
        --jq ".[] | select(.displayTitle | contains(\"${TOKEN}\")) | [.databaseId, .headSha] | @tsv" \
        | head -n1 || true)"
    if [[ -n "${RUN_ID}" ]]; then
        RUN_SHA="${RUN_ID#*$'\t'}"
        RUN_ID="${RUN_ID%%$'\t'*}"
        break
    fi
    if (( SECONDS >= deadline )); then
        echo "build.sh: ERROR: no run with token ${TOKEN} appeared within ${FIND_TIMEOUT_SECS}s" >&2
        echo "Check the Actions tab for ${WORKFLOW} on ${BRANCH}." >&2
        exit 1
    fi
    sleep "${FIND_INTERVAL_SECS}"
done

RUN_URL="$(gh run view "${RUN_ID}" --json url --jq .url)"
echo "== build.sh: run ${RUN_ID}: ${RUN_URL} =="
if [[ "${RUN_SHA}" != "${HEAD_SHA}" ]]; then
    echo "build.sh: ERROR: run ${RUN_ID} built ${RUN_SHA:0:10}, expected ${HEAD_SHA:0:10}: ${RUN_URL}" >&2
    exit 1
fi

# gh run watch also exits on a transient API error (GitHub answers 502 at times), which would
# leave the run going while the artifacts below are fetched too early. So its exit status is
# ignored: the run's own status is polled until it completes, and its conclusion decides.
gh run watch "${RUN_ID}" --interval "${WATCH_INTERVAL_SECS}" || true

RUN_CONCLUSION=""
while true; do
    RUN_STATE="$(gh run view "${RUN_ID}" --json status,conclusion --jq '.status + " " + .conclusion' 2>/dev/null || true)"
    if [[ "${RUN_STATE%% *}" == "completed" ]]; then
        RUN_CONCLUSION="${RUN_STATE#* }"
        break
    fi
    sleep "${WATCH_INTERVAL_SECS}"
done

RUN_STATUS=0
if [[ "${RUN_CONCLUSION}" != "success" ]]; then
    echo "build.sh: run ${RUN_ID} concluded '${RUN_CONCLUSION}'" >&2
    RUN_STATUS=1
fi

OUT_DIR="${REPO_ROOT}/build/remote/${PRESET}"
rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}"

# A missing artifact (step skipped, run cancelled early) must not replace the
# run's own status, so a failed download only reports. A few attempts ride out
# transient API errors.
download_artifact() {
    local name="$1"
    local dir="${2:-${OUT_DIR}}"
    local staging="${OUT_DIR:?}/.download-${name}"
    for _ in 1 2 3; do
        rm -rf "${staging:?}"
        if gh run download "${RUN_ID}" -n "${name}" -D "${staging}" >/dev/null 2>&1; then
            mkdir -p "${dir}"
            cp -R "${staging}/." "${dir}/"
            rm -rf "${staging:?}"
            return 0
        fi
        sleep "${FIND_INTERVAL_SECS}"
    done
    rm -rf "${staging:?}"
    echo "build.sh: artifact ${name} was not available for download" >&2
    return 1
}

APPIMAGE=""
CTEST_LOG_DIR=""

if [[ "${WANT_APPIMAGE}" == "true" ]]; then
    if download_artifact "natron-${PRESET}-${HEAD_SHA}"; then
        APPIMAGE="$(find "${OUT_DIR}" -maxdepth 1 -name '*.AppImage' -print -quit)"
        if [[ -n "${APPIMAGE}" ]]; then
            chmod +x "${APPIMAGE}"
        fi
    fi
fi

if [[ "${RUN_TESTS}" == "true" ]]; then
    if download_artifact "ctest-logs-${PRESET}-${HEAD_SHA}"; then
        CTEST_LOG_DIR="${OUT_DIR}"
    fi
fi

GOLDEN_DIR=""
if [[ "${RECORD_GOLDENS}" == "true" ]]; then
    GOLDEN_DIR="${REPO_ROOT}/Tests/fixtures/native-golden"
    GOLDEN_TMP="${OUT_DIR}/native-goldens"
    mkdir -p "${GOLDEN_TMP}"
    if download_artifact "native-goldens-${PRESET}-${HEAD_SHA}" "${GOLDEN_TMP}"; then
        mkdir -p "${GOLDEN_DIR}"
        cp -R "${GOLDEN_TMP}/." "${GOLDEN_DIR}/"
    else
        GOLDEN_DIR=""
        RUN_STATUS=1
    fi
fi

if [[ "${RUN_STATUS}" -eq 0 ]]; then
    echo "== build.sh: remote build succeeded: ${RUN_URL} =="
    if [[ -n "${APPIMAGE}" ]]; then
        echo "AppImage: ${APPIMAGE}"
    elif [[ "${WANT_APPIMAGE}" == "true" ]]; then
        echo "build.sh: run succeeded but no AppImage was downloaded" >&2
        RUN_STATUS=1
    fi
else
    echo "== build.sh: remote build FAILED (exit ${RUN_STATUS}): ${RUN_URL} ==" >&2
fi
if [[ -n "${GOLDEN_DIR}" ]]; then
    echo "native goldens: ${GOLDEN_DIR}"
fi
if [[ -n "${CTEST_LOG_DIR}" ]]; then
    echo "ctest logs: ${CTEST_LOG_DIR}"
    if [[ -n "${SHUFFLE_SEEDS}" ]]; then
        for log in "${CTEST_LOG_DIR}"/ctest-*-seed*.log; do
            if [[ -f "${log}" ]]; then
                echo "shuffled ctest log: ${log}"
            fi
        done
    fi
fi

exit "${RUN_STATUS}"
