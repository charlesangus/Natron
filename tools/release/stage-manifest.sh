#!/usr/bin/env bash
# Write a deterministic manifest of a staged bundle: one sorted line per path.
# Fields are tab separated: path, octal mode, type, symlink target, sha256.
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "Usage: stage-manifest.sh <staged dir> [out file]" >&2
    exit 1
fi

STAGE_DIR="$(cd "$1" && pwd)"
OUT="${2:-/dev/stdout}"

cd "$STAGE_DIR"
export LC_ALL=C

find . -mindepth 1 -printf '%P\0' | sort -z | while IFS= read -r -d '' path; do
    mode="$(stat -c '%a' -- "$path")"
    target=""
    hash=""
    if [[ -L "$path" ]]; then
        type=symlink
        target="$(readlink -- "$path")"
    elif [[ -d "$path" ]]; then
        type=dir
    elif [[ -f "$path" ]]; then
        type=file
        hash="$(sha256sum -- "$path" | cut -d' ' -f1)"
    else
        type=other
    fi
    printf '%s\t%s\t%s\t%s\t%s\n' "$path" "$mode" "$type" "$target" "$hash"
done > "$OUT"
