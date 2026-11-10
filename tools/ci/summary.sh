#!/usr/bin/env bash
# Usage: summary.sh <subcommand> ...
#
#   build <preset> <artifact[,artifact]> [cmake-arg...]
#                                              preset, commit, CMake args, uploaded artifacts
#   ctest <scheduler> <build-dir> <log-artifact> [heading-label]
#                                              pass/fail/total counts and failed test names
#   gui <script> <result> <artifact>           script, result, output artifact
#   legs <title> <name>=<result>...            one line per job or matrix leg
#   note <title> <line>...                     a heading and free-form lines
#
# Appends Markdown to $GITHUB_STEP_SUMMARY (stdout when unset). It runs as an
# `if: always()` step, often after a failure that left the inputs half-written,
# so every missing file is reported as "unknown" and the exit status is always 0.
#
# JOB_STATUS (the job's `job.status`), when set, is shown as the result.
set -u

FAILED_CAP=50
BT=$'\x60'

out() {
    if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
        cat >>"${GITHUB_STEP_SUMMARY}" 2>/dev/null || cat
    else
        cat
    fi
}

or_none() {
    if [[ -n "${1:-}" ]]; then
        printf '%s' "$1"
    else
        printf 'none'
    fi
}

# A comma-separated list of artifact names, each in code formatting.
artifact_list() {
    if [[ -z "${1:-}" ]]; then
        printf 'none'
        return
    fi
    printf "${BT}%s${BT}" "${1//,/${BT}, ${BT}}"
}

status_line() {
    if [[ -n "${JOB_STATUS:-}" ]]; then
        printf -- '- Result: **%s**\n' "${JOB_STATUS}"
    fi
}

cmd_build() {
    local preset="${1:-unknown}" artifact="${2:-}"
    shift $(($# > 2 ? 2 : $#))
    local sha="${GITHUB_SHA:-}"
    {
        printf '### Build · %s\n\n' "${preset}"
        status_line
        printf -- "- Preset: ${BT}%s${BT}\n" "${preset}"
        printf -- "- Commit: ${BT}%s${BT}\n" "$(or_none "${sha::8}")"
        if [[ $# -gt 0 ]]; then
            printf -- "- CMake args: ${BT}%s${BT}\n" "$*"
        else
            printf -- '- CMake args: none\n'
        fi
        printf -- '- Artifact: %s\n' "$(artifact_list "${artifact}")"
    } | out
}

# "93% tests passed, 3 tests failed out of 40" -> "37 3 40"
counts_from_stdout() {
    local file="$1" line failed total
    [[ -r "${file}" ]] || return 1
    line="$(grep -E 'tests passed, [0-9]+ tests? failed out of [0-9]+' "${file}" 2>/dev/null | tail -n 1)"
    [[ -n "${line}" ]] || return 1
    failed="$(sed -E 's/.*tests passed, ([0-9]+) tests? failed out of ([0-9]+).*/\1/' <<<"${line}")"
    total="$(sed -E 's/.*tests passed, ([0-9]+) tests? failed out of ([0-9]+).*/\2/' <<<"${line}")"
    printf '%s %s %s' "$((total - failed))" "${failed}" "${total}"
}

# Used when no stdout capture exists (serial `ctest -V` runs).
counts_from_last_test() {
    local file="$1" passed failed
    [[ -r "${file}" ]] || return 1
    passed="$(grep -c '^Test Passed\.' "${file}" 2>/dev/null)"
    failed="$(grep -c '^Test Failed\.' "${file}" 2>/dev/null)"
    [[ "$((passed + failed))" -gt 0 ]] || return 1
    printf '%s %s %s' "${passed}" "${failed}" "$((passed + failed))"
}

# LastTestsFailed.log holds "<index>:<name>" lines; the stdout capture's
# "The following tests FAILED:" list is the fallback, since a serial re-run
# of the failures rewrites the former.
failed_names() {
    local temp="$1/Testing/Temporary" names=""
    if [[ -s "${temp}/LastTestsFailed.log" ]]; then
        names="$(sed -E 's/^[0-9]+://; s/\r$//' "${temp}/LastTestsFailed.log" 2>/dev/null)"
    fi
    if [[ -z "${names}" && -r "${temp}/ctest-stdout.log" ]]; then
        names="$(awk '
            /^The following tests FAILED:/ { in_list = 1; next }
            in_list && /^[ \t]+[0-9]+ - / {
                sub(/^[ \t]+[0-9]+ - /, ""); sub(/ \([^(]*\)$/, ""); print; next
            }
            in_list { in_list = 0 }
        ' "${temp}/ctest-stdout.log" 2>/dev/null)"
    fi
    printf '%s' "${names}"
}

cmd_ctest() {
    local scheduler="${1:-unknown}" dir="${2:-}" log_artifact="${3:-}" label="${4:-}"
    local temp="${dir}/Testing/Temporary" counts="" passed failed total names shown
    counts="$(counts_from_stdout "${temp}/ctest-stdout.log")" ||
        counts="$(counts_from_last_test "${temp}/LastTest.log")" || counts=""
    names="$(failed_names "${dir}")"
    {
        printf '### Test · %s scheduler\n\n' "${label:-${scheduler}}"
        status_line
        printf -- "- Scheduler: ${BT}%s${BT}\n" "${scheduler}"
        if [[ -n "${counts}" ]]; then
            read -r passed failed total <<<"${counts}"
            printf -- '- Passed: %s, failed: %s, total: %s\n' "${passed}" "${failed}" "${total}"
        else
            printf -- "- Passed / failed / total: unknown (no ctest output found in ${BT}%s${BT})\n" "${dir}"
        fi
        if [[ -n "${log_artifact}" ]]; then
            printf -- "- Log artifact: ${BT}%s${BT}\n" "${log_artifact}"
        fi
        if [[ -n "${names}" ]]; then
            total="$(grep -c . <<<"${names}")"
            printf '\nFailed tests:\n\n'
            shown="$(head -n "${FAILED_CAP}" <<<"${names}")"
            while IFS= read -r name; do
                printf -- "- ${BT}%s${BT}\n" "${name}"
            done <<<"${shown}"
            if ((total > FAILED_CAP)); then
                printf -- '- ... and %d more' "$((total - FAILED_CAP))"
                if [[ -n "${log_artifact}" ]]; then
                    printf " (see ${BT}%s${BT})" "${log_artifact}"
                fi
                printf '\n'
            fi
        fi
    } | out
}

cmd_gui() {
    local script="${1:-unknown}" result="${2:-}" artifact="${3:-}"
    {
        printf '### GUI · %s\n\n' "${script}"
        printf -- "- Script: ${BT}%s${BT}\n" "${script}"
        printf -- '- Result: **%s**\n' "$(or_none "${result}")"
        printf -- '- Screenshots and output: %s\n' "$(artifact_list "${artifact}")"
    } | out
}

cmd_legs() {
    local title="${1:-Results}" leg
    shift $(($# > 0 ? 1 : 0))
    {
        printf '### %s\n\n' "${title}"
        for leg in "$@"; do
            printf -- '- %s: **%s**\n' "${leg%%=*}" "$(or_none "${leg#*=}")"
        done
    } | out
}

cmd_note() {
    local title="${1:-Summary}" line
    shift $(($# > 0 ? 1 : 0))
    {
        printf '### %s\n\n' "${title}"
        for line in "$@"; do
            printf -- '- %s\n' "${line}"
        done
    } | out
}

main() {
    local sub="${1:-}"
    shift $(($# > 0 ? 1 : 0))
    case "${sub}" in
    build) cmd_build "$@" ;;
    ctest) cmd_ctest "$@" ;;
    gui) cmd_gui "$@" ;;
    legs) cmd_legs "$@" ;;
    note) cmd_note "$@" ;;
    *) echo "summary.sh: unknown subcommand '${sub}'" >&2 ;;
    esac
}

main "$@"
exit 0
