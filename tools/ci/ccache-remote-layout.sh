#!/usr/bin/env bash
# Sourced by the remote workflow and by tools/ci/local/devshell.sh so both
# address the same ccache remote key namespace whatever spelling the stored
# URL uses.

# Prints the storage value with any layout attribute replaced by bazel: the
# built-in http client takes `layout=bazel`, the https helper takes the custom
# attribute `@layout=bazel` and otherwise defaults to a different layout.
# Other values pass through unchanged.
ccache_remote_with_bazel_layout() {
    local value="$1" stripped
    case "${value}" in
        https://* | http://*) ;;
        *)
            printf '%s' "${value}"
            return 0
            ;;
    esac
    stripped="$(printf '%s' "${value}" | sed -E 's/[|[:space:]]+@?layout=[^|[:space:]]*//g; s/[|[:space:]]+$//')"
    case "${value}" in
        https://*) printf '%s|@layout=bazel' "${stripped}" ;;
        *) printf '%s|layout=bazel' "${stripped}" ;;
    esac
}
