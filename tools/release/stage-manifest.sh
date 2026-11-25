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

# Resolve the out file against the caller's cwd before changing directory, and
# refuse one inside the stage dir: it would be listed in its own manifest.
if [[ $# -eq 2 ]]; then
    OUT="$(realpath -m -- "$OUT")"
    STAGE_REAL="$(realpath -- "$STAGE_DIR")"
    if [[ "$OUT" == "$STAGE_REAL"/* ]]; then
        echo "error: out file $OUT lies inside the staged dir $STAGE_REAL" >&2
        exit 1
    fi
fi

cd "$STAGE_DIR"
export LC_ALL=C

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

find . -mindepth 1 -printf '%P\0%m\0%y\0%l\0' > "$WORK_DIR/meta"

# sha256sum -z neither escapes the name nor appends a newline, so the name is
# everything after the hash and its two separator characters. One sha256sum at
# a time: parallel ones would interleave their buffered output.
if ! find . -mindepth 1 -type f -printf '%P\0' |
    xargs -0 -r sha256sum -z -- > "$WORK_DIR/hashes"; then
    echo "error: could not hash every file under $STAGE_DIR" >&2
    exit 1
fi

declare -A MODE TYPE TARGET HASH
while IFS= read -r -d '' path && IFS= read -r -d '' mode &&
      IFS= read -r -d '' kind && IFS= read -r -d '' target; do
    MODE["$path"]="$mode"
    case "$kind" in
        l) TYPE["$path"]=symlink ;;
        d) TYPE["$path"]=dir ;;
        f) TYPE["$path"]="file" ;;
        *) TYPE["$path"]=other ;;
    esac
    TARGET["$path"]="$target"
done < "$WORK_DIR/meta"

# Matches plain sha256sum output, which marks a name holding a backslash or a
# newline by prefixing a backslash to the hash.
while IFS= read -r -d '' line; do
    path="${line:66}"
    if [[ "$path" == *$'\\'* || "$path" == *$'\n'* ]]; then
        HASH["$path"]="\\${line:0:64}"
    else
        HASH["$path"]="${line:0:64}"
    fi
done < "$WORK_DIR/hashes"

find . -mindepth 1 -printf '%P\0' | sort -z | while IFS= read -r -d '' path; do
    printf '%s\t%s\t%s\t%s\t%s\n' "$path" "${MODE[$path]}" "${TYPE[$path]}" \
        "${TARGET[$path]}" "${HASH[$path]:-}"
done > "$OUT"
