#!/usr/bin/env bash
#
# Poll every recent remote-build.yml run with one API call per interval and
# publish the results as files, so any number of tools/ci/remote/build.sh
# instances can wait on their runs without calling the API themselves.
#
# Usage:
#   tools/ci/remote/watch-runs.sh <state-dir>
#
# Writes, each atomically:
#   <state-dir>/<run-id>     "<status> <conclusion>" of each listed run
#   <state-dir>/titles.tsv   "<run-id>\t<head-sha>\t<display-title>" per listed run
#   <state-dir>/heartbeat    "<epoch-secs> <interval-secs>", rewritten each poll
#
# Only one watcher runs per state dir (flock on <state-dir>/watcher.lock); a
# second one exits 0 at once. build.sh registers itself as
# <state-dir>/clients/<pid>; entries whose process is gone are dropped. The
# watcher exits after NATRON_REMOTE_WATCHER_IDLE_SECS (default 600) with no
# unfinished run in the list and no registered client. The heartbeat is
# removed on exit, and the heartbeat is only written after a successful poll.
# Clients and the watcher must share a host and PID namespace (liveness is
# kill -0), and one state dir serves one repo. State files of runs that left
# the list, and stale .tmp.* files, are removed each poll.
#
#   NATRON_REMOTE_WATCH_INTERVAL_SECS  seconds between polls (default 60)

set -euo pipefail

WORKFLOW="remote-build.yml"
LIST_LIMIT=100

if [[ $# -ne 1 || -z "$1" ]]; then
    echo "usage: $0 <state-dir>" >&2
    exit 2
fi

STATE_DIR="$1"
INTERVAL_SECS="${NATRON_REMOTE_WATCH_INTERVAL_SECS:-60}"
IDLE_SECS="${NATRON_REMOTE_WATCHER_IDLE_SECS:-600}"
for value in "${INTERVAL_SECS}" "${IDLE_SECS}"; do
    if [[ ! "${value}" =~ ^[1-9][0-9]*$ ]]; then
        echo "watch-runs.sh: intervals must be positive whole numbers of seconds, got '${value}'" >&2
        exit 2
    fi
done

CLIENTS_DIR="${STATE_DIR}/clients"
mkdir -p "${CLIENTS_DIR}"

exec 9>"${STATE_DIR}/watcher.lock"
if ! flock -n 9; then
    exit 0
fi

trap 'rm -f "${STATE_DIR:?}/heartbeat"' EXIT
trap 'exit 0' INT TERM

write_atomic() {
    local target="$1"
    local content="$2"
    local tmp
    tmp="$(mktemp "${STATE_DIR}/.tmp.XXXXXX")"
    printf '%s' "${content}" >"${tmp}"
    mv -f "${tmp}" "${target}"
}

live_clients() {
    local count=0
    local entry pid
    for entry in "${CLIENTS_DIR}"/*; do
        [[ -e "${entry}" ]] || continue
        pid="${entry##*/}"
        if [[ "${pid}" =~ ^[0-9]+$ ]] && kill -0 "${pid}" 2>/dev/null; then
            count=$((count + 1))
        else
            rm -f "${CLIENTS_DIR:?}/${pid}"
        fi
    done
    echo "${count}"
}

prune_state() {
    local listed="$1"
    local name
    while IFS= read -r name; do
        [[ -n "${name}" ]] || continue
        if [[ "${name}" == .tmp.* ]]; then
            rm -f "${STATE_DIR:?}/${name}"
        elif [[ "${name}" =~ ^[0-9]+$ && "${listed}" != *$'\n'"${name}"$'\n'* ]]; then
            rm -f "${STATE_DIR:?}/${name}"
        fi
    done < <(find "${STATE_DIR}" -maxdepth 1 -type f -printf '%f\n')
}

last_active="${SECONDS}"
while true; do
    unfinished=0
    poll_ok=false
    if listing="$(timeout "$((2 * INTERVAL_SECS))" gh run list --workflow "${WORKFLOW}" --limit "${LIST_LIMIT}" \
        --json databaseId,status,conclusion,displayTitle,headSha \
        --jq '.[] | [.databaseId, .status, (if (.conclusion // "") == "" then "-" else .conclusion end), .headSha, .displayTitle] | @tsv' \
        2>&1 9>&-)"; then
        titles=""
        listed=$'\n'
        # Tab is whitespace to read, so consecutive tabs collapse: an empty conclusion
        # travels as "-" to keep the fields aligned.
        while IFS=$'\t' read -r run_id status conclusion head_sha title; do
            [[ "${run_id}" =~ ^[0-9]+$ ]] || continue
            [[ "${conclusion}" != "-" ]] || conclusion=""
            listed+="${run_id}"$'\n'
            titles+="${run_id}"$'\t'"${head_sha}"$'\t'"${title}"$'\n'
            state="${status} ${conclusion}"
            if [[ "$(cat "${STATE_DIR}/${run_id}" 2>/dev/null || true)" != "${state}" ]]; then
                write_atomic "${STATE_DIR}/${run_id}" "${state}"
            fi
            if [[ "${status}" != "completed" ]]; then
                unfinished=$((unfinished + 1))
            fi
        done <<<"${listing}"
        write_atomic "${STATE_DIR}/titles.tsv" "${titles}"
        prune_state "${listed}"
        poll_ok=true
    else
        # The last good state stays in place; a poll that failed says nothing about idleness.
        echo "watch-runs.sh: $(date -u +%FT%TZ) gh run list failed: ${listing}" >&2
        unfinished=1
    fi
    if [[ "${poll_ok}" == "true" ]]; then
        write_atomic "${STATE_DIR}/heartbeat" "$(date +%s) ${INTERVAL_SECS}"
    fi

    if (( unfinished > 0 )) || (( $(live_clients) > 0 )); then
        last_active="${SECONDS}"
    elif (( SECONDS - last_active >= IDLE_SECS )); then
        rm -f "${STATE_DIR:?}/heartbeat"
        if (( $(live_clients) == 0 )); then
            exit 0
        fi
        last_active="${SECONDS}"
    fi
    sleep "${INTERVAL_SECS}" 9>&-
done
