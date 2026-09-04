#!/usr/bin/env bash
# Pack a staged bundle into a versioned, checksummed .tar.xz release tarball.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CMAKELISTS="$SCRIPT_DIR/../../CMakeLists.txt"

if [[ $# -ne 2 ]]; then
    echo "Usage: make-tarball.sh <staging-dir> <output-dir>" >&2
    echo "" >&2
    echo "Example: tools/release/make-tarball.sh /tmp/natron-stage /tmp/artifacts" >&2
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

for tool in tar xz sha256sum; do
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

BUNDLE_NAME="Natron-${VERSION}-linux-x86_64"
TARBALL="$OUTPUT_DIR/${BUNDLE_NAME}.tar.xz"
CHECKSUM="$TARBALL.sha256"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

ln -s "$STAGING_DIR" "$WORK_DIR/$BUNDLE_NAME"

echo "==> Creating $TARBALL"
tar --dereference -cJf "$TARBALL" -C "$WORK_DIR" "$BUNDLE_NAME"

echo "==> Writing checksum $CHECKSUM"
( cd "$OUTPUT_DIR" && sha256sum "$(basename "$TARBALL")" > "$(basename "$CHECKSUM")" )

echo "$TARBALL"
echo "$CHECKSUM"
