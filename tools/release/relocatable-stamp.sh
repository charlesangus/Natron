#!/usr/bin/env bash
# Record, or test, that check-relocatable.sh passed on a staged tree exactly as
# it is now. The stamp sits beside the staging dir, never in it, so the bundle
# is unchanged by it. It is the hash of the tree's manifest, its absolute path
# (an absolute symlink resolves differently once the tree moves) and the
# checker and excludelist that judged it, so any edit to the tree or to the
# check makes it stale.
#
#   relocatable-stamp.sh clear <staging-dir>
#   relocatable-stamp.sh write <staging-dir>
#   relocatable-stamp.sh check <staging-dir>   exit 0 only if the stamp matches
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ $# -ne 2 || ! "$1" =~ ^(clear|write|check)$ ]]; then
    echo "Usage: relocatable-stamp.sh clear|write|check <staging-dir>" >&2
    exit 2
fi

ACTION="$1"
STAGE_DIR="$(cd "$2" && pwd -P)"
STAMP="$STAGE_DIR.relocatable"

fingerprint() {
    {
        printf 'stage %s\n' "$STAGE_DIR"
        sha256sum < "$SCRIPT_DIR/check-relocatable.sh"
        sha256sum < "$SCRIPT_DIR/excludelist.txt"
        "$SCRIPT_DIR/stage-manifest.sh" "$STAGE_DIR"
    } | sha256sum | cut -d' ' -f1
}

case "$ACTION" in
    clear)
        rm -f -- "$STAMP"
        ;;
    write)
        fingerprint > "$STAMP"
        ;;
    check)
        [[ -f "$STAMP" ]] || exit 1
        current="$(fingerprint)" || exit 1
        [[ "$current" == "$(< "$STAMP")" ]]
        ;;
esac
