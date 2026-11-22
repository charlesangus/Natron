#!/usr/bin/env bash
# Verify that a staged Natron bundle resolves its own dependencies without
# help from the host: every DT_NEEDED entry of every staged ELF must be
# satisfiable from inside the staging tree, or be on the excludelist of
# libraries the bundle deliberately takes from the host.
#
# ldd is not usable for this. It runs the real loader, so it also consults
# ld.so.cache and the system library directories, and on a build machine that
# has the ASWF VFX libraries and Qt's xcb dependencies installed system-wide
# it reports a bundle as complete when it is not. This reads DT_NEEDED and
# DT_RUNPATH/DT_RPATH straight out of the ELF headers and resolves them by
# hand against the bundle alone, so it returns the same verdict on a build
# machine as on a bare desktop.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SELF="$SCRIPT_DIR/${BASH_SOURCE[0]##*/}"
EXCLUDELIST="$SCRIPT_DIR/excludelist.txt"

# Internal: "--batch <staging-dir> <list> <out>" checks the NUL-separated files
# in <list> and writes "C <count>" and "U <binary>: <soname>" lines to <out>.
BATCH_MODE=0
if [[ "${1:-}" == "--batch" && $# -eq 4 ]]; then
    BATCH_MODE=1
    BATCH_LIST="$3"
    BATCH_OUT="$4"
    set -- "$2"
fi

if [[ $# -ne 1 ]]; then
    echo "Usage: check-relocatable.sh <staging-dir>" >&2
    echo "" >&2
    echo "Example: tools/release/check-relocatable.sh /tmp/natron-stage" >&2
    exit 1
fi

STAGE_DIR="$1"

if [[ ! -d "$STAGE_DIR" ]]; then
    echo "error: staging dir $STAGE_DIR does not exist" >&2
    exit 1
fi

for tool in objdump file readlink xargs; do
    if ! command -v "$tool" &>/dev/null; then
        echo "error: $tool not found in PATH" >&2
        exit 1
    fi
done

STAGE_DIR="$(cd "$STAGE_DIR" && pwd)"

load_excludelist() {
    local -n _set=$1
    while IFS= read -r line; do
        line="${line%%#*}"
        line="${line// /}"
        [[ -z "$line" ]] && continue
        _set["$line"]=1
    done < "$EXCLUDELIST"
}

declare -A EXCLUDED
load_excludelist EXCLUDED

is_excluded() {
    local lib_basename="$1"
    [[ -v EXCLUDED["$lib_basename"] ]] && return 0
    for pattern in "${!EXCLUDED[@]}"; do
        if [[ "$pattern" == *'*'* ]]; then
            # shellcheck disable=SC2254
            case "$lib_basename" in $pattern) return 0 ;; esac
        fi
    done
    return 1
}

# lib/optional/* holds the GCC runtime libraries the bundle carries but does
# not impose: nothing's RUNPATH reaches them, and bin/natron-runtime.sh names
# whichever of the bundled and the host's copy is newer on LD_LIBRARY_PATH,
# which ld.so searches ahead of DT_RUNPATH. They are still inside the bundle,
# so they count as resolved -- for every binary, since LD_LIBRARY_PATH is not
# per-file.
OPTIONAL_ENTRIES=()
for optional_dir in "$STAGE_DIR"/lib/optional/*/; do
    if [[ -d "$optional_dir" ]]; then
        OPTIONAL_ENTRIES+=("${optional_dir%/}")
    fi
done

# One objdump per binary. ld.so ignores DT_RPATH entirely once DT_RUNPATH is
# present, so mirror that precedence rather than concatenating the two.
ELF_NEEDED=()
ELF_SEARCH_PATH=""
read_dynamic() {
    local key value rest runpath="" rpath="" have_runpath=0 have_rpath=0
    ELF_NEEDED=()
    while read -r key value rest; do
        case "$key" in
            NEEDED) ELF_NEEDED+=("$value") ;;
            RUNPATH) if [[ "$have_runpath" == 0 ]]; then runpath="$value"; have_runpath=1; fi ;;
            RPATH) if [[ "$have_rpath" == 0 ]]; then rpath="$value"; have_rpath=1; fi ;;
        esac
    done < <(objdump -p "$1" 2>/dev/null)
    if [[ -n "$runpath" ]]; then
        ELF_SEARCH_PATH="$runpath"
    else
        ELF_SEARCH_PATH="$rpath"
    fi
}

# readlink -f of <dir>/<name>, left in CANONICAL_FILE. Only a symlink, or a
# name with a directory part, costs a readlink of its own; each directory is
# canonicalised once.
declare -A CANONICAL_DIR
CANONICAL_FILE=""
canonical_file() {
    local dir="$1" name="$2"
    if [[ "$name" == */* || -L "$dir/$name" ]]; then
        CANONICAL_FILE="$(readlink -f "$dir/$name")"
        return
    fi
    if [[ ! -v CANONICAL_DIR["$dir"] ]]; then
        CANONICAL_DIR["$dir"]="$(readlink -f "$dir")"
    fi
    if [[ "${CANONICAL_DIR[$dir]}" == / ]]; then
        CANONICAL_FILE="/$name"
    else
        CANONICAL_FILE="${CANONICAL_DIR[$dir]}/$name"
    fi
}

# Resolution deliberately uses only the binary's own DT_RUNPATH/DT_RPATH. The
# real loader also lets a DT_RPATH on an earlier link in the chain rescue a
# library that carries none, but DT_RUNPATH -- which is what patchelf writes,
# and what every modern toolchain emits -- does not chain that way, so a
# bundle that relies on inheritance is broken for exactly the libraries this
# check exists to catch.
resolve_in_bundle() {
    local binary="$1" soname="$2"
    local origin entry
    origin="${binary%/*}"

    local entries=()
    IFS=':' read -r -a entries <<< "$ELF_SEARCH_PATH"
    entries+=("${OPTIONAL_ENTRIES[@]}")

    for entry in "${entries[@]}"; do
        [[ -z "$entry" ]] && continue
        entry="${entry//\$\{ORIGIN\}/$origin}"
        entry="${entry//\$ORIGIN/$origin}"
        [[ -f "$entry/$soname" ]] || continue
        canonical_file "$entry" "$soname"
        # A RUNPATH may point out of the staging tree -- Qt's stock plugin
        # RUNPATH does exactly that. Such a hit is a host library, not a
        # bundled one, and must not count as resolved.
        [[ "$CANONICAL_FILE" == "$STAGE_DIR"/* ]] || continue
        return 0
    done

    return 1
}

check_batch() {
    local -a files types
    local i binary soname checked=0

    mapfile -d '' -t files < "$BATCH_LIST"
    [[ ${#files[@]} -gt 0 ]] || { echo "C 0" > "$BATCH_OUT"; return 0; }
    mapfile -t types < <(file -b -- "${files[@]}" 2>/dev/null)
    if [[ ${#types[@]} -ne ${#files[@]} ]]; then
        echo "error: file(1) gave ${#types[@]} answers for ${#files[@]} files" >&2
        return 1
    fi

    {
        for (( i = 0; i < ${#files[@]}; i++ )); do
            [[ "${types[i]}" == ELF* ]] || continue
            binary="${files[i]}"
            checked=$((checked + 1))
            read_dynamic "$binary"
            for soname in "${ELF_NEEDED[@]}"; do
                [[ -z "$soname" ]] && continue
                if is_excluded "$soname"; then
                    continue
                fi
                if ! resolve_in_bundle "$binary" "$soname"; then
                    printf 'U %s: %s\n' "${binary#"$STAGE_DIR"/}" "$soname"
                fi
            done
        done
        printf 'C %s\n' "$checked"
    } > "$BATCH_OUT.partial"
    mv -f "$BATCH_OUT.partial" "$BATCH_OUT"
}

if [[ "$BATCH_MODE" == 1 ]]; then
    check_batch
    exit
fi

echo "==> Resolving staged binaries against $STAGE_DIR alone"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

JOBS="${NATRON_CHECK_JOBS:-$(nproc)}"
mapfile -d '' -t ALL_FILES < <(find "$STAGE_DIR" -type f -print0 | LC_ALL=C sort -z)
BATCH_SIZE=$(( (${#ALL_FILES[@]} + JOBS * 4 - 1) / (JOBS * 4) ))
(( BATCH_SIZE > 0 )) || BATCH_SIZE=1

BATCHES=()
for (( start = 0; start < ${#ALL_FILES[@]}; start += BATCH_SIZE )); do
    batch="$WORK_DIR/$(printf '%06d' "${#BATCHES[@]}")"
    printf '%s\0' "${ALL_FILES[@]:start:BATCH_SIZE}" > "$batch"
    BATCHES+=("$batch")
done

if [[ ${#BATCHES[@]} -gt 0 ]]; then
    if ! printf '%s\0' "${BATCHES[@]}" |
        xargs -0 -P "$JOBS" -I{} "$SELF" --batch "$STAGE_DIR" {} {}.out; then
        echo "error: a checker batch failed; the bundle was not verified" >&2
        exit 1
    fi
fi

UNRESOLVED=()
CHECKED=0
for batch in "${BATCHES[@]}"; do
    if [[ ! -f "$batch.out" ]]; then
        echo "error: a checker batch produced no result; the bundle was not verified" >&2
        exit 1
    fi
    while IFS= read -r line; do
        case "$line" in
            "C "*) CHECKED=$((CHECKED + ${line#C })) ;;
            "U "*) UNRESOLVED+=("${line#U }") ;;
        esac
    done < "$batch.out"
done

if [[ ${#UNRESOLVED[@]} -gt 0 ]]; then
    echo "error: ${#UNRESOLVED[@]} dependencies of $CHECKED staged binaries cannot be resolved from the bundle:" >&2
    printf '  %s\n' "${UNRESOLVED[@]}" >&2
    echo "" >&2
    echo "Each is neither staged with a RUNPATH that reaches it nor listed in" >&2
    echo "$EXCLUDELIST as host-provided. The bundle is not relocatable." >&2
    exit 1
fi

echo "==> Relocatable: $CHECKED staged binaries resolve within the bundle"
