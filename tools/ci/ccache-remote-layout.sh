#!/usr/bin/env bash
# Sourced by the remote workflow and by tools/ci/local/devshell.sh so both
# address the same ccache remote key namespace whatever spelling the stored
# URL uses.

# Prints the storage value with any layout attribute replaced by bazel: the
# built-in http client takes `layout=bazel`, the https helper takes the custom
# attribute `@layout=bazel` and otherwise defaults to a different layout.
# A leading `url=` or `docker_url=` key, surrounding whitespace and quotes are
# dropped, since the value is often copied from the local config line. Prints
# nothing for anything that is not an http:// or https:// URL, so a bad value
# turns the remote cache off instead of failing every compile.
ccache_remote_with_bazel_layout() {
    local value="$1" stripped
    case "${value}" in
        *$'\n'* | *$'\r'*) return 0 ;;
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

# Prints the bare host of a normalised storage value (no credentials, port,
# path or attributes), for masking it separately from the whole value.
ccache_remote_host() {
    local rest="${1#*://}"
    rest="${rest%%|*}"
    rest="${rest%%/*}"
    rest="${rest##*@}"
    rest="${rest%%:*}"
    printf '%s' "${rest}"
}
