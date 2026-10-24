#!/usr/bin/env bash
#
# One memory cap for everything this repo runs locally, sized per host, so
# a runaway build, test or render is OOM-killed on its own instead of
# swapping the whole machine until it stops responding.
#
# Usage:
#   tools/ci/local/memcap.sh --docker-args   # print `docker run`/`docker update` flags
#   tools/ci/local/memcap.sh --mib [--docker] # print the cap in MiB (0 = no cap)
#   tools/ci/local/memcap.sh <cmd> [args...] # run a host command under the cap
#
# The cap defaults to 75% of the memory the target sees, keeping at least
# 4 GiB back for the desktop and everything else. "The target" is the Docker
# daemon's MemTotal for --docker-args (which is the VM's memory under Docker
# Desktop, not the Mac's) and /proc/meminfo for host commands. Override with
# NATRON_DEV_MEMORY, as a whole number with an optional b/k/m/g suffix (e.g.
# 12g, 8192m; nothing else is accepted) or as a literal "0" to turn the cap
# off. A nonzero value that rounds to 0 MiB is an error, not "off".
#
# The one exception to the 4 GiB reserve: on a host with under 8 GiB, half the
# memory goes to the workload instead, since the reserve would leave next to
# none.
#
# Swap is capped at the same value as memory (`--memory-swap` equal to
# `--memory`, `MemorySwapMax=0`): a capped process that swaps instead of dying
# still takes the host with it.
#
# Host commands run in a transient systemd user scope. Where there is none
# (macOS, a container, no user session), the command runs uncapped and a
# warning says so.

set -euo pipefail

usage() {
    sed -n '/^# Usage:/,/^# The cap/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//' >&2
    exit 2
}

[[ "$#" -gt 0 ]] || usage

total_mib() {
    local bytes
    if [[ "$1" == "docker" ]]; then
        bytes="$(docker info --format '{{.MemTotal}}')"
    elif [[ -r /proc/meminfo ]]; then
        bytes="$(awk '/^MemTotal:/ { print $2 * 1024 }' /proc/meminfo)"
    else
        bytes="$(sysctl -n hw.memsize)"
    fi
    echo $(( bytes / 1048576 ))
}

to_mib() {
    local v n unit mib
    v="$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')"
    n="${v%%[a-z]*}"
    unit="${v#"${n}"}"
    [[ -n "${n}" && "${n}" =~ ^[0-9]+$ ]] || { echo "memcap.sh: bad NATRON_DEV_MEMORY '${1}'" >&2; exit 2; }
    case "${unit}" in
        g|gb|gi|gib) mib=$(( n * 1024 )) ;;
        m|mb|mi|mib) mib="${n}" ;;
        k|kb|ki|kib) mib=$(( n / 1024 )) ;;
        ''|b) mib=$(( n / 1048576 )) ;;
        *) echo "memcap.sh: bad NATRON_DEV_MEMORY unit '${1}'" >&2; exit 2 ;;
    esac
    if (( mib == 0 )); then
        echo "memcap.sh: NATRON_DEV_MEMORY '${1}' rounds to 0 MiB; use a literal 0 to turn the cap off" >&2
        exit 2
    fi
    echo "${mib}"
}

cap_mib() {
    if [[ "${NATRON_DEV_MEMORY:-}" == "0" ]]; then
        echo 0
        return
    fi
    if [[ -n "${NATRON_DEV_MEMORY:-}" ]]; then
        to_mib "${NATRON_DEV_MEMORY}"
        return
    fi
    local total reserve
    total="$(total_mib "$1")"
    reserve=$(( total / 4 ))
    (( reserve >= 4096 )) || reserve=4096
    # A host with under 8 GiB gets half its memory rather than next to none.
    if (( total - reserve < total / 2 )); then
        echo $(( total / 2 ))
    else
        echo $(( total - reserve ))
    fi
}

case "$1" in
    --docker-args)
        mib="$(cap_mib docker)"
        (( mib > 0 )) && echo "--memory=${mib}m --memory-swap=${mib}m"
        exit 0
        ;;
    --mib)
        if [[ "${2:-}" == "--docker" ]]; then cap_mib docker; else cap_mib host; fi
        exit 0
        ;;
    -h|--help)
        usage
        ;;
esac

mib="$(cap_mib host)"
if (( mib == 0 )); then
    exec "$@"
fi
if ! command -v systemd-run >/dev/null 2>&1 \
    || ! systemctl --user show-environment >/dev/null 2>&1; then
    echo "memcap.sh: no systemd user session here; running '$1' without a memory cap" >&2
    exec "$@"
fi
exec systemd-run --user --scope --quiet --collect \
    -p MemoryMax="${mib}M" -p MemorySwapMax=0 \
    -- "$@"
