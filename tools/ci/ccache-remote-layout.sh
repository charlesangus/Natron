#!/usr/bin/env bash
# Sourced by the remote workflow and by tools/ci/local/devshell.sh so both
# address the same ccache remote key namespace whatever spelling the stored
# URL uses.

# Prints the storage value with its layout attribute forced to bazel, or
# nothing when it is not an http(s) URL. The http client spells the attribute
# `layout=bazel` and the https helper `@layout=bazel`. Only url= is used from a
# pasted config: CI ignores docker_url because runners cannot reach it.
ccache_remote_with_bazel_layout() {
    local value="${1//$'\r'/}" stripped line key picked="" keyless="" keyless_count=0
    value="${value#"${value%%[![:space:]]*}"}"
    value="${value%"${value##*[![:space:]]}"}"
    case "${value}" in
        *$'\n'*)
            while IFS= read -r line || [[ -n "${line}" ]]; do
                line="${line#"${line%%[![:space:]]*}"}"
                line="${line%"${line##*[![:space:]]}"}"
                case "${line}" in
                    "" | "#"*) continue ;;
                    http://* | https://*)
                        keyless="${line}"
                        keyless_count=$((keyless_count + 1))
                        continue
                        ;;
                esac
                [[ "${line}" == *=* ]] || continue
                key="${line%%=*}"
                key="${key//[[:space:]]/}"
                if [[ "${key}" == "url" ]]; then
                    picked="${line#*=}"
                fi
            done <<<"${value}"
            if [[ -z "${picked}" && "${keyless_count}" -eq 1 ]]; then
                picked="${keyless}"
            fi
            value="${picked}"
            ;;
    esac
    value="${value#"${value%%[![:space:]]*}"}"
    value="${value%"${value##*[![:space:]]}"}"
    value="${value#[\"\']}"
    value="${value%[\"\']}"
    value="${value#"${value%%[![:space:]]*}"}"
    value="${value%"${value##*[![:space:]]}"}"
    case "${value}" in
        url=*) value="${value#url=}" ;;
        docker_url=*) value="${value#docker_url=}" ;;
    esac
    value="${value#"${value%%[![:space:]]*}"}"
    value="${value#[\"\']}"
    value="${value%[\"\']}"
    value="${value%"${value##*[![:space:]]}"}"
    case "${value}" in
        https://* | http://*) ;;
        *) return 0 ;;
    esac
    case "${value}" in
        *$'\n'*) return 0 ;;
    esac
    stripped="$(printf '%s' "${value}" | sed -E 's/[|[:space:]]+@?layout=[^|[:space:]]*//g; s/[|[:space:]]+$//')"
    case "${stripped}" in
        https://?* | http://?*) ;;
        *) return 0 ;;
    esac
    case "${value}" in
        https://*) printf '%s|@layout=bazel' "${stripped}" ;;
        *) printf '%s|layout=bazel' "${stripped}" ;;
    esac
}

# Prints the bare host (an IPv6 literal keeps its brackets) of a normalised
# storage value, for masking it separately from the whole value.
ccache_remote_host() {
    local rest="${1#*://}"
    rest="${rest%%|*}"
    rest="${rest%%[/?#]*}"
    rest="${rest##*@}"
    if [[ "${rest}" == \[* ]]; then
        rest="${rest%%]*}]"
    else
        rest="${rest%%:*}"
    fi
    printf '%s' "${rest}"
}
