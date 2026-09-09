#!/usr/bin/env bash
# Run check-startup.sh against a staged bundle inside a container that never
# had the ASWF/Conan VFX stack installed -- see container/Dockerfile.
#
# check-startup.sh run inside aswf/ci-vfxall can pass on a bundle that only
# works because that image has the VFX stack installed system-wide; this
# runs the same script, unmodified, somewhere it does not.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="natron-startup-check:debian12"

if [[ $# -ne 1 ]]; then
    echo "Usage: check-startup-container.sh <staging-dir>" >&2
    echo "" >&2
    echo "Example: tools/release/check-startup-container.sh /tmp/natron-stage" >&2
    exit 1
fi

STAGE_DIR="$1"

if [[ ! -d "$STAGE_DIR" ]]; then
    echo "error: staging dir $STAGE_DIR does not exist" >&2
    exit 1
fi

if ! command -v docker &>/dev/null; then
    echo "error: docker not found in PATH" >&2
    exit 1
fi

STAGE_DIR="$(cd "$STAGE_DIR" && pwd)"

echo "==> Building $IMAGE (cached after the first run)"
docker build -q -t "$IMAGE" "$SCRIPT_DIR/container" >/dev/null

echo "==> Running check-startup.sh inside $IMAGE against $STAGE_DIR"
exec docker run --rm \
    -v "$SCRIPT_DIR:/opt/release:ro" \
    -v "$STAGE_DIR:/bundle:ro" \
    "$IMAGE" \
    bash /opt/release/check-startup.sh /bundle
