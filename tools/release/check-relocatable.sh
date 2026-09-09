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
EXCLUDELIST="$SCRIPT_DIR/excludelist.txt"

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

for tool in objdump file; do
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

collect_binaries() {
    while IFS= read -r f; do
        if file -b "$f" 2>/dev/null | grep -q "^ELF"; then
            echo "$f"
        fi
    done < <(find "$STAGE_DIR" -type f | sort)
}

needed_of() {
    objdump -p "$1" 2>/dev/null | awk '$1 == "NEEDED" { print $2 }'
}

# ld.so ignores DT_RPATH entirely once DT_RUNPATH is present, so mirror that
# precedence rather than concatenating the two.
search_path_of() {
    local dump runpath
    dump="$(objdump -p "$1" 2>/dev/null)"
    runpath="$(awk '$1 == "RUNPATH" { print $2; exit }' <<< "$dump")"
    if [[ -n "$runpath" ]]; then
        echo "$runpath"
    else
        awk '$1 == "RPATH" { print $2; exit }' <<< "$dump"
    fi
}

# lib/optional/* holds the GCC runtime libraries the bundle carries but does
# not impose: nothing's RUNPATH reaches them, and bin/natron-runtime.sh names
# whichever of the bundled and the host's copy is newer on LD_LIBRARY_PATH,
# which ld.so searches ahead of DT_RUNPATH. They are still inside the bundle,
# so they count as resolved -- for every binary, since LD_LIBRARY_PATH is not
# per-file.
optional_entries() {
    local dir
    for dir in "$STAGE_DIR"/lib/optional/*/; do
        [[ -d "$dir" ]] && printf '%s\n' "${dir%/}"
    done
}

OPTIONAL_ENTRIES=()
while IFS= read -r optional_dir; do
    [[ -n "$optional_dir" ]] && OPTIONAL_ENTRIES+=("$optional_dir")
done < <(optional_entries)

# Resolution deliberately uses only the binary's own DT_RUNPATH/DT_RPATH. The
# real loader also lets a DT_RPATH on an earlier link in the chain rescue a
# library that carries none, but DT_RUNPATH -- which is what patchelf writes,
# and what every modern toolchain emits -- does not chain that way, so a
# bundle that relies on inheritance is broken for exactly the libraries this
# check exists to catch.
resolve_in_bundle() {
    local binary="$1" soname="$2"
    local origin entry candidate resolved
    origin="$(dirname "$binary")"

    local entries=()
    IFS=':' read -r -a entries <<< "$(search_path_of "$binary")"
    entries+=("${OPTIONAL_ENTRIES[@]}")

    for entry in "${entries[@]}"; do
        [[ -z "$entry" ]] && continue
        entry="${entry//\$\{ORIGIN\}/$origin}"
        entry="${entry//\$ORIGIN/$origin}"
        candidate="$entry/$soname"
        [[ -f "$candidate" ]] || continue
        resolved="$(readlink -f "$candidate")"
        # A RUNPATH may point out of the staging tree -- Qt's stock plugin
        # RUNPATH does exactly that. Such a hit is a host library, not a
        # bundled one, and must not count as resolved.
        [[ "$resolved" == "$STAGE_DIR"/* ]] || continue
        echo "$resolved"
        return 0
    done

    return 1
}

echo "==> Resolving staged binaries against $STAGE_DIR alone"

UNRESOLVED=()
CHECKED=0

while IFS= read -r binary; do
    CHECKED=$((CHECKED + 1))
    while IFS= read -r soname; do
        [[ -z "$soname" ]] && continue
        if is_excluded "$soname"; then
            continue
        fi
        if ! resolve_in_bundle "$binary" "$soname" > /dev/null; then
            UNRESOLVED+=("${binary#"$STAGE_DIR"/}: $soname")
        fi
    done < <(needed_of "$binary")
done < <(collect_binaries)

if [[ ${#UNRESOLVED[@]} -gt 0 ]]; then
    echo "error: ${#UNRESOLVED[@]} dependencies of $CHECKED staged binaries cannot be resolved from the bundle:" >&2
    printf '  %s\n' "${UNRESOLVED[@]}" >&2
    echo "" >&2
    echo "Each is neither staged with a RUNPATH that reaches it nor listed in" >&2
    echo "$EXCLUDELIST as host-provided. The bundle is not relocatable." >&2
    exit 1
fi

echo "==> Relocatable: $CHECKED staged binaries resolve within the bundle"
