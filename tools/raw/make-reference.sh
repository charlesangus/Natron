#!/usr/bin/env bash
# Builds darktable-cli at DARKTABLE_REF (tools/ci/local/fetch-assets.sh) in a throwaway Docker
# image and renders the darktable references of Tests/fixtures/raw/reference with it: the
# developed crops RawReference_Test compares the Develop chain with, and the synthetic clipped
# mosaics and per-mode results NativeRawHighlights_Test compares RawHighlights with.
#
# Usage: tools/raw/make-reference.sh [--jobs N] [--keep-image]
#
# The camera raw fixtures must be checked out (git lfs pull). The image is removed afterwards
# unless --keep-image is given; a later run with the image still present skips the build.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
JOBS=4
KEEP_IMAGE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --jobs) JOBS="$2"; shift 2 ;;
        --keep-image) KEEP_IMAGE=1; shift ;;
        *) echo "usage: $0 [--jobs N] [--keep-image]" >&2; exit 2 ;;
    esac
done

pin() {
    sed -n "s/^$1=\"\(.*\)\"$/\1/p" "${ROOT}/tools/ci/local/fetch-assets.sh"
}
DARKTABLE_REPO="$(pin DARKTABLE_REPO)"
DARKTABLE_REF="$(pin DARKTABLE_REF)"
if [ -z "${DARKTABLE_REPO}" ] || [ -z "${DARKTABLE_REF}" ]; then
    echo "DARKTABLE_REPO or DARKTABLE_REF not found in tools/ci/local/fetch-assets.sh" >&2
    exit 1
fi
IMAGE="natron-darktable-ref:${DARKTABLE_REF:0:12}"

FIXTURES="${ROOT}/Tests/fixtures/raw"
OUT="${FIXTURES}/reference"
for f in canon-eos-5d-mark-iii.cr2 fujifilm-x-pro2-14bit-uncompressed.raf; do
    # An LFS pointer is a few hundred bytes of text.
    if [ "$(stat -c %s "${FIXTURES}/${f}")" -lt 1000000 ]; then
        echo "${FIXTURES}/${f} is not checked out; run git lfs pull" >&2
        exit 1
    fi
done

if ! docker image inspect "${IMAGE}" > /dev/null 2>&1; then
    CONTEXT="$(mktemp -d)"
    docker build -f "${ROOT}/tools/raw/darktable-ref.Dockerfile" \
        --build-arg DARKTABLE_REPO="${DARKTABLE_REPO}" \
        --build-arg DARKTABLE_REF="${DARKTABLE_REF}" \
        --build-arg JOBS="${JOBS}" \
        -t "${IMAGE}" "${CONTEXT}"
    rmdir "${CONTEXT}"
fi

mkdir -p "${OUT}"
docker run --rm --user "$(id -u):$(id -g)" \
    -e HOME=/tmp -e DARKTABLE_REF="${DARKTABLE_REF:0:12}" \
    -v "${ROOT}/tools/raw:/tools:ro" \
    -v "${FIXTURES}:/fixtures:ro" \
    -v "${OUT}:/out" \
    --tmpfs /scratch:exec,size=2g \
    "${IMAGE}" \
    python3 /tools/make-reference.py /fixtures /out /scratch

if [ "${KEEP_IMAGE}" = 0 ]; then
    docker image rm "${IMAGE}" > /dev/null
fi
ls -l "${OUT}"
