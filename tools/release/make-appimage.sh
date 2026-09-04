#!/usr/bin/env bash
# Build the AppDir and the AppImage from a staged Natron bundle tree.
# The AppImage runtime needs FUSE2 at run time; if it is unavailable, run the AppImage with --appimage-extract-and-run.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CMAKELISTS="$SCRIPT_DIR/../../CMakeLists.txt"
APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"

if [[ $# -ne 2 ]]; then
    echo "Usage: make-appimage.sh <staging-dir> <output-dir>" >&2
    echo "" >&2
    echo "Example: tools/release/make-appimage.sh /tmp/natron-stage /tmp/artifacts" >&2
    exit 1
fi

STAGING_DIR="$1"
OUTPUT_DIR="$2"

if [[ ! -d "$STAGING_DIR" ]]; then
    echo "error: staging dir $STAGING_DIR does not exist" >&2
    exit 1
fi

if [[ ! -f "$CMAKELISTS" ]]; then
    echo "error: cannot find CMakeLists.txt at $CMAKELISTS" >&2
    exit 1
fi

for tool in sha256sum curl; do
    if ! command -v "$tool" &>/dev/null; then
        echo "error: $tool not found in PATH" >&2
        exit 1
    fi
done

VERSION="$(grep -m1 'VERSION "' "$CMAKELISTS" | sed -n 's/.*VERSION "\([^"]*\)".*/\1/p')"

if [[ -z "$VERSION" ]]; then
    echo "error: could not parse VERSION from $CMAKELISTS" >&2
    exit 1
fi

mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"
STAGING_DIR="$(cd "$STAGING_DIR" && pwd)"

DESKTOP_FILE="$STAGING_DIR/share/applications/fr.natron.Natron.desktop"
ICON_FILE="$STAGING_DIR/share/pixmaps/natronIcon256_linux.png"
APPDATA_FILE="$STAGING_DIR/share/metainfo/fr.natron.Natron.appdata.xml"

for f in "$DESKTOP_FILE" "$ICON_FILE" "$APPDATA_FILE"; do
    if [[ ! -f "$f" ]]; then
        echo "error: expected staged file not found: $f" >&2
        exit 1
    fi
done

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

APPDIR="$WORK_DIR/Natron.AppDir"

echo "==> Building AppDir at $APPDIR"
mkdir -p "$APPDIR"
cp -a "$STAGING_DIR"/. "$APPDIR"/

cp "$DESKTOP_FILE" "$APPDIR/fr.natron.Natron.desktop"
cp "$ICON_FILE" "$APPDIR/natronIcon256_linux.png"

mkdir -p "$APPDIR/usr/share/metainfo"
cp "$APPDATA_FILE" "$APPDIR/usr/share/metainfo/fr.natron.Natron.appdata.xml"

cat > "$APPDIR/AppRun" <<'APPRUN'
#!/bin/bash
HERE="$(dirname "$(readlink -f "$0")")"
export PATH="$HERE/bin:$PATH"
exec "$HERE/bin/Natron" "$@"
APPRUN
chmod +x "$APPDIR/AppRun"

APPIMAGETOOL=""
if command -v appimagetool &>/dev/null; then
    APPIMAGETOOL="$(command -v appimagetool)"
else
    echo "==> Downloading appimagetool"
    APPIMAGETOOL="$WORK_DIR/appimagetool-x86_64.AppImage"
    curl -fL -o "$APPIMAGETOOL" "$APPIMAGETOOL_URL"
    chmod +x "$APPIMAGETOOL"
fi

BUNDLE_NAME="Natron-${VERSION}-x86_64"
APPIMAGE="$OUTPUT_DIR/${BUNDLE_NAME}.AppImage"
CHECKSUM="$APPIMAGE.sha256"

echo "==> Running appimagetool"
ARCH=x86_64 "$APPIMAGETOOL" "$APPDIR" "$APPIMAGE"

echo "==> Writing checksum $CHECKSUM"
( cd "$OUTPUT_DIR" && sha256sum "$(basename "$APPIMAGE")" > "$(basename "$CHECKSUM")" )

echo "$APPIMAGE"
echo "$CHECKSUM"
