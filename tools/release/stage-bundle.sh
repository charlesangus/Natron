#!/usr/bin/env bash
# Stage a relocatable Natron bundle from a CMake build directory.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXCLUDELIST="$SCRIPT_DIR/excludelist.txt"
CHECK_RELOCATABLE="$SCRIPT_DIR/check-relocatable.sh"
CHECK_STARTUP="$SCRIPT_DIR/check-startup.sh"

if [[ $# -ne 2 ]]; then
    echo "Usage: stage-bundle.sh <build-dir> <staging-dir>" >&2
    exit 1
fi

BUILD_DIR="$(cd "$1" && pwd)"
STAGE_DIR="$2"

for tool in patchelf ldd file tar; do
    if ! command -v "$tool" &>/dev/null; then
        echo "error: $tool not found in PATH" >&2
        exit 1
    fi
done

if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    echo "error: $BUILD_DIR does not look like a CMake build directory (no CMakeCache.txt)" >&2
    exit 1
fi

cache_value() {
    awk -F= -v key="$1" '$1 == key { print $2; exit }' "$BUILD_DIR/CMakeCache.txt"
}

# The source tree and the interpreter both come from the CMake cache rather than
# from this script's own location or from PATH: the resources staged below have
# to be the ones this build was configured against, and the bundled stdlib has
# to belong to the very libpython the binaries are linked to. A second Python on
# PATH would produce a bundle whose stdlib and libpython disagree.
SOURCE_DIR="$(cache_value 'CMAKE_HOME_DIRECTORY:INTERNAL')"
if [[ -z "$SOURCE_DIR" || ! -d "$SOURCE_DIR" ]]; then
    echo "error: cannot determine the source tree of $BUILD_DIR from its CMakeCache.txt" >&2
    exit 1
fi

PYTHON_EXE="$(cache_value 'Python3_EXECUTABLE:FILEPATH')"
if [[ -z "$PYTHON_EXE" ]]; then
    PYTHON_EXE="$(cache_value '_Python3_EXECUTABLE:INTERNAL')"
fi
if [[ -z "$PYTHON_EXE" || ! -x "$PYTHON_EXE" ]]; then
    echo "error: cannot determine the Python interpreter $BUILD_DIR was configured against" >&2
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

# Copy a directory without the bytecode caches, and without needing to delete
# anything back out of the staging tree afterwards. Extra tar --exclude
# patterns may be passed after the destination.
copy_tree() {
    local src="$1" dest="$2"
    shift 2
    mkdir -p "$dest"
    tar -C "$src" --exclude='__pycache__' "$@" -cf - . | tar -C "$dest" -xf -
}

read -r PY_TAG PY_STDLIB PY_SITELIB < <(
    "$PYTHON_EXE" -c 'import sys, sysconfig
print("%d.%d" % sys.version_info[:2], sysconfig.get_path("stdlib"), sysconfig.get_path("purelib"))'
)

if [[ -z "$PY_TAG" || ! -d "$PY_STDLIB" ]]; then
    echo "error: $PYTHON_EXE reported no usable standard library location" >&2
    exit 1
fi

# Global/PythonUtils.cpp resolves the Python home as <bin>/.., and only accepts
# it once lib/python<X.Y> exists beside the binaries; it then puts that
# directory, its lib-dynload and its site-packages on PYTHONPATH. Staging into
# exactly that layout is what makes the shipped interpreter start from the
# bundle instead of dying on "No module named 'encodings'".
PY_DEST="$STAGE_DIR/lib/python$PY_TAG"

echo "==> Staging the Python $PY_TAG standard library from $PY_STDLIB"

# config-* is the static libpython and Makefile fragments used to *build*
# extension modules, which a bundle never does and which alone outweighs
# everything else here. ensurepip installs packages into a tree that is
# read-only in an AppImage. tkinter and _tkinter would drag Tcl/Tk in for a
# toolkit Natron does not use, idlelib is its editor, and test/turtledemo are
# not shipped software. site-packages is staged selectively below.
copy_tree "$PY_STDLIB" "$PY_DEST" \
    --exclude='./config-*' \
    --exclude='./ensurepip' \
    --exclude='./idlelib' \
    --exclude='./site-packages' \
    --exclude='./test' \
    --exclude='./tkinter' \
    --exclude='./turtledemo' \
    --exclude='_tkinter*'

mkdir -p "$PY_DEST/site-packages"

# Natron's Python layer imports exactly these: AppManager::loadPythonGroups()
# runs "import PySide6" then "from PySide6 import QtCore" and, outside
# background mode, "from PySide6 import QtGui"; PyPlugs build their panels on
# QtWidgets. PySide6/__init__.py resolves shiboken6 as its sibling, so that
# comes too. The rest of PySide6 is left out deliberately -- each remaining
# binding pulls its own Qt module into the closure walk below, and the bundle
# ships neither Qt Quick nor Qt Multimedia nor Qt WebEngine.
PYSIDE_SRC="$PY_SITELIB/PySide6"
if [[ -d "$PYSIDE_SRC" ]]; then
    echo "==> Staging PySide6 (QtCore, QtGui, QtWidgets) and shiboken6"
    mkdir -p "$PY_DEST/site-packages/PySide6"
    for pyside_item in __init__.py _config.py _git_pyside_version.py support \
                       QtCore.abi3.so QtGui.abi3.so QtWidgets.abi3.so; do
        if [[ -e "$PYSIDE_SRC/$pyside_item" ]]; then
            cp -a "$PYSIDE_SRC/$pyside_item" "$PY_DEST/site-packages/PySide6/"
        fi
    done
    if [[ -d "$PY_SITELIB/shiboken6" ]]; then
        copy_tree "$PY_SITELIB/shiboken6" "$PY_DEST/site-packages/shiboken6"
    fi
else
    echo "warning: no PySide6 in $PY_SITELIB — Natron's Python GUI layer will not load" >&2
fi

while IFS= read -r staged_py_binary; do
    set_bundle_runpath "$staged_py_binary"
done < <(find "$PY_DEST" -type f -name "*.so" 2>/dev/null)

echo "==> Staging bundle-relative resources"

# AppManager::loadInternal() points FONTCONFIG_PATH at <bin>/../Resources/etc/
# fonts. The template is stored with its quotes backslash-escaped, which is not
# well-formed XML: unescape as well as substitute, or fontconfig rejects the
# file it was finally given and falls back to the host's.
FONTS_DEST="$STAGE_DIR/Resources/etc/fonts"
FONTS_SRC="$SOURCE_DIR/Gui/Resources/etc/fonts"
if [[ ! -f "$FONTS_SRC/fonts.conf.in" ]]; then
    echo "error: $FONTS_SRC/fonts.conf.in not found" >&2
    exit 1
fi

mkdir -p "$FONTS_DEST"
sed -e 's/\\"/"/g' \
    -e "s/\\\\'/'/g" \
    -e 's|[$][$]FC_DEFAULT_FONTS|<dir>/usr/share/fonts</dir>\n\t<dir>/usr/local/share/fonts</dir>|' \
    -e 's|[$][$]FC_CACHEDIR|<cachedir>/var/cache/fontconfig</cachedir>|' \
    "$FONTS_SRC/fonts.conf.in" > "$FONTS_DEST/fonts.conf"

copy_tree "$FONTS_SRC/conf.d" "$FONTS_DEST/conf.d"

# AppManager::getAllNonOFXPluginsPaths() looks for PyPlugs under
# <bin>/../Plugins/PyPlugs, and OfxHost under <bin>/../Plugins/OFX/Natron.
copy_tree "$SOURCE_DIR/Gui/Resources/PyPlugs" "$STAGE_DIR/Plugins/PyPlugs"

# Plugins/OFX/Natron is left empty on purpose. The prebuilt OFX bundles ship
# their own Contents/Libraries with RUNPATHs written for the tree they were
# built in, and dropping them in here unmodified leaves them unable to resolve
# either those libraries or the bundle's own lib/. Relocating them needs a
# RUNPATH rewrite that preserves each bundle's internal layout, which is its
# own piece of work; until then this ships no OFX plugins rather than broken
# ones, and check-relocatable.sh will say so if that changes.

# Settings::getDefaultOcioConfigPaths() searches <bin>/../share/OpenColorIO-
# Configs and <bin>/../Resources/OpenColorIO-Configs for the named on-disk
# configs. Natron's default is the built-in "ocio://" URI, which libOpenColorIO
# resolves without any files at all, so an absent asset tree is not an error --
# only the named legacy configs become unselectable.
OCIO_ASSETS="$BUILD_DIR/assets/OpenColorIO-Configs"
if [[ -d "$OCIO_ASSETS" ]]; then
    echo "==> Staging OpenColorIO configs from $OCIO_ASSETS"
    copy_tree "$OCIO_ASSETS" "$STAGE_DIR/Resources/OpenColorIO-Configs"
else
    echo "==> No $OCIO_ASSETS — shipping the built-in OpenColorIO config only"
fi

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

# The check above reads ELF headers, so a bundle missing a data dependency --
# the Python standard library, the fontconfig configuration -- passes it and
# still cannot start. Actually start the binaries.
echo "==> Verifying the staged binaries start"
"$CHECK_STARTUP" "$STAGE_DIR"

echo "==> Staging complete: $STAGE_DIR"
