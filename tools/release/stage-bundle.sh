#!/usr/bin/env bash
# Stage a relocatable Natron bundle from a CMake build directory.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXCLUDELIST="$SCRIPT_DIR/excludelist.txt"

if [[ $# -ne 2 ]]; then
    echo "Usage: stage-bundle.sh <build-dir> <staging-dir>" >&2
    exit 1
fi

BUILD_DIR="$(cd "$1" && pwd)"
STAGE_DIR="$2"

for tool in patchelf ldd; do
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

echo "==> Installing from CMake build to $STAGE_DIR"
cmake --install "$BUILD_DIR" --prefix "$STAGE_DIR"

mkdir -p "$STAGE_DIR/lib"

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

collect_elfs() {
    local dir="$1"
    local pattern="${2:-*}"
    find "$dir" -type f -name "$pattern" 2>/dev/null
}

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
declare -A COPIED

collect_binaries() {
    while IFS= read -r f; do
        file -b "$f" 2>/dev/null | grep -q "ELF" && echo "$f"
    done < <(collect_elfs "$STAGE_DIR/bin")
    find "$STAGE_DIR/Plugins/OFX/Natron" -name "*.ofx" -type f 2>/dev/null || true
    collect_elfs "$STAGE_DIR/plugins" "*.so" 2>/dev/null || true
}

is_inside_ofx_libraries() {
    local path="$1"
    [[ "$path" == */Contents/Libraries/* ]] && return 0
    return 1
}

walk_closure() {
    local queue=()
    while IFS= read -r bin; do
        queue+=("$bin")
    done < <(collect_binaries)

    while [[ ${#queue[@]} -gt 0 ]]; do
        local current="${queue[0]}"
        queue=("${queue[@]:1}")

        [[ -v SEEN["$current"] ]] && continue
        SEEN["$current"]=1

        while IFS= read -r dep; do
            [[ -z "$dep" ]] && continue
            [[ -v SEEN["$dep"] ]] && continue

            local dep_basename
            dep_basename="$(basename "$dep")"

            if is_excluded "$dep_basename"; then
                continue
            fi

            if is_inside_ofx_libraries "$dep"; then
                continue
            fi

            local already_in_stage=0
            if [[ -f "$STAGE_DIR/lib/$dep_basename" ]]; then
                already_in_stage=1
            fi

            if [[ "$dep" == "$STAGE_DIR"/* ]]; then
                already_in_stage=1
            fi

            if [[ $already_in_stage -eq 0 && -f "$dep" ]]; then
                if [[ ! -v COPIED["$dep_basename"] ]]; then
                    cp -L "$dep" "$STAGE_DIR/lib/$dep_basename"
                    COPIED["$dep_basename"]=1
                    echo "  copied: $dep_basename"
                fi
            fi

            queue+=("$STAGE_DIR/lib/$dep_basename")
        done < <(resolve_deps "$current")
    done
}

walk_closure

echo "==> Setting RUNPATH on binaries"
for bin in "$STAGE_DIR"/bin/*; do
    [[ -f "$bin" ]] || continue
    file -b "$bin" 2>/dev/null | grep -q "ELF" || continue
    [[ "$(basename "$bin")" == "qt.conf" ]] && continue
    patchelf --set-rpath '$ORIGIN/../lib' "$bin"
    echo "  patched: $(basename "$bin")"
done

echo "==> Verification pass"
ERRORS=0

verify_binary() {
    local binary="$1"
    local missing
    missing="$(ldd "$binary" 2>/dev/null | grep "not found" || true)"
    if [[ -n "$missing" ]]; then
        echo "MISSING deps for $binary:" >&2
        echo "$missing" >&2
        ERRORS=$((ERRORS + 1))
    fi
}

for bin in "$STAGE_DIR"/bin/*; do
    [[ -f "$bin" ]] || continue
    file -b "$bin" 2>/dev/null | grep -q "ELF" || continue
    [[ "$(basename "$bin")" == "qt.conf" ]] && continue
    verify_binary "$bin"
done

while IFS= read -r lib; do
    verify_binary "$lib"
done < <(find "$STAGE_DIR/lib" -name "*.so*" -type f 2>/dev/null)

while IFS= read -r plugin; do
    verify_binary "$plugin"
done < <(find "$STAGE_DIR/plugins" -name "*.so" -type f 2>/dev/null)

while IFS= read -r ofx; do
    verify_binary "$ofx"
done < <(find "$STAGE_DIR/Plugins" -name "*.ofx" -type f 2>/dev/null)

if [[ $ERRORS -gt 0 ]]; then
    echo "error: $ERRORS binaries have unresolved dependencies" >&2
    exit 1
fi

echo "==> Staging complete: $STAGE_DIR"
