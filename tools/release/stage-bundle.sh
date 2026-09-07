#!/usr/bin/env bash
# Stage a relocatable Natron bundle from a CMake build directory.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXCLUDELIST="$SCRIPT_DIR/excludelist.txt"
CHECK_RELOCATABLE="$SCRIPT_DIR/check-relocatable.sh"

if [[ $# -ne 2 ]]; then
    echo "Usage: stage-bundle.sh <build-dir> <staging-dir>" >&2
    exit 1
fi

BUILD_DIR="$(cd "$1" && pwd)"
STAGE_DIR="$2"

for tool in patchelf ldd file; do
    if ! command -v "$tool" &>/dev/null; then
        echo "error: $tool not found in PATH" >&2
        exit 1
    fi
done

if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    echo "error: $BUILD_DIR does not look like a CMake build directory (no CMakeCache.txt)" >&2
    exit 1
fi

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

# Absolute, so that "did this dependency resolve to something already inside
# the bundle?" is answerable by prefix match no matter what the caller passed.
mkdir -p "$STAGE_DIR"
STAGE_DIR="$(cd "$STAGE_DIR" && pwd)"

is_elf() {
    file -b "$1" 2>/dev/null | grep -q "^ELF"
}

# DT_RUNPATH is not transitive: it governs only the DT_NEEDED entries of the
# file that carries it, not anything further down the chain. So every staged
# ELF needs its own way back to lib/, and the number of ".." depends on where
# in the tree that file landed -- plugins/platforms/libqxcb.so needs
# $ORIGIN/../../lib where bin/Natron needs $ORIGIN/../lib.
set_bundle_runpath() {
    local target="$1"
    local dir rel depth up=""
    local i

    is_elf "$target" || return 0

    dir="$(cd "$(dirname "$target")" && pwd)"
    rel="${dir#"$STAGE_DIR"}"
    rel="${rel#/}"

    if [[ -n "$rel" ]]; then
        depth="$(awk -F/ '{ print NF }' <<< "$rel")"
        for (( i = 0; i < depth; i++ )); do
            up+="../"
        done
    fi

    chmod u+w "$target"
    patchelf --set-rpath "\$ORIGIN/${up}lib" "$target"
}

stage_library() {
    local src="$1" name="$2"
    cp -L "$src" "$STAGE_DIR/lib/$name"
    set_bundle_runpath "$STAGE_DIR/lib/$name"
}

echo "==> Installing from CMake build to $STAGE_DIR"
cmake --install "$BUILD_DIR" --prefix "$STAGE_DIR"

mkdir -p "$STAGE_DIR/lib"

while IFS= read -r installed; do
    set_bundle_runpath "$installed"
done < <(find "$STAGE_DIR/bin" "$STAGE_DIR/lib" -type f 2>/dev/null)

QT_PLUGIN_DIR=""
QT_PLUGIN_DIR="$(qmake6 -query QT_INSTALL_PLUGINS 2>/dev/null || qtpaths6 --plugin-dir 2>/dev/null || true)"
if [[ -z "$QT_PLUGIN_DIR" || ! -d "$QT_PLUGIN_DIR" ]]; then
    echo "error: cannot determine Qt6 plugin directory" >&2
    exit 1
fi

echo "==> Copying Qt6 plugins from $QT_PLUGIN_DIR"

for subdir in platforms imageformats xcbglintegrations; do
    src="$QT_PLUGIN_DIR/$subdir"
    if [[ -d "$src" ]]; then
        mkdir -p "$STAGE_DIR/plugins/$subdir"
        cp -a "$src"/*.so "$STAGE_DIR/plugins/$subdir/" 2>/dev/null || true
        while IFS= read -r plugin; do
            set_bundle_runpath "$plugin"
        done < <(find "$STAGE_DIR/plugins/$subdir" -type f -name "*.so" 2>/dev/null)
    fi
done

if [[ ! -f "$STAGE_DIR/plugins/platforms/libqxcb.so" ]]; then
    echo "warning: libqxcb.so not found in Qt plugins — XCB platform may be missing" >&2
fi

echo "==> Writing qt.conf"
cat > "$STAGE_DIR/bin/qt.conf" <<'QTCONF'
[Paths]
Prefix = ..
Plugins = plugins
QTCONF

resolve_deps() {
    local binary="$1"
    ldd "$binary" 2>/dev/null | while read -r line; do
        if [[ "$line" == *"=>"* ]]; then
            local path
            path="$(echo "$line" | sed -n 's/.* => \(\/[^ ]*\) .*/\1/p')"
            if [[ -n "$path" ]]; then
                echo "$path"
            fi
        fi
    done
}

echo "==> Walking ldd closure"

declare -A SEEN

# Seed from everything the bundle ships that the loader can be asked for:
# the executables, the libraries CMake installed, and the whole Qt plugin
# tree -- plugins are dlopen'd, so nothing in bin/ lists them as a
# dependency and a walk seeded from bin/ alone never reaches what they need.
collect_binaries() {
    while IFS= read -r f; do
        if is_elf "$f"; then
            echo "$f"
        fi
    done < <(find "$STAGE_DIR/bin" "$STAGE_DIR/lib" "$STAGE_DIR/plugins" -type f 2>/dev/null
             find "$STAGE_DIR/Plugins" -type f -name "*.ofx" 2>/dev/null)
}

is_inside_ofx_libraries() {
    local path="$1"
    [[ "$path" == */Contents/Libraries/* ]] && return 0
    return 1
}

walk_closure() {
    local queue=()
    local current dep dep_basename staged

    while IFS= read -r bin; do
        queue+=("$bin")
    done < <(collect_binaries)

    while [[ ${#queue[@]} -gt 0 ]]; do
        current="${queue[0]}"
        queue=("${queue[@]:1}")

        [[ -v SEEN["$current"] ]] && continue
        SEEN["$current"]=1

        while IFS= read -r dep; do
            [[ -z "$dep" ]] && continue

            dep_basename="$(basename "$dep")"

            if is_excluded "$dep_basename"; then
                continue
            fi

            if is_inside_ofx_libraries "$dep"; then
                continue
            fi

            if [[ "$dep" == "$STAGE_DIR"/* ]]; then
                staged="$dep"
            else
                staged="$STAGE_DIR/lib/$dep_basename"
                if [[ ! -f "$staged" ]]; then
                    stage_library "$dep" "$dep_basename"
                    echo "  copied: $dep_basename"
                fi
            fi

            # Everything staged goes back on the queue and gets walked in
            # turn, so a library pulled in by a plugin contributes its own
            # dependencies too. SEEN makes each file resolve once, which is
            # what brings the walk to a fixed point.
            queue+=("$staged")
        done < <(resolve_deps "$current")
    done
}

walk_closure

# ldd is the right tool above -- finding host libraries to bundle is exactly
# its job -- and the wrong one here, because it would answer using this
# machine's ld.so.cache. Hand the finished tree to the checker instead.
echo "==> Verifying the staged tree resolves against itself"
"$CHECK_RELOCATABLE" "$STAGE_DIR"

echo "==> Staging complete: $STAGE_DIR"
