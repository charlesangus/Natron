#!/usr/bin/env bash
# Builds darktable-cli at DARKTABLE_REF (tools/ci/local/fetch-assets.sh) in a throwaway Docker
# image and renders the darktable references of Tests/fixtures/raw/reference with it: the
# developed crops RawReference_Test compares the Develop chain with, and the synthetic clipped
# mosaics and per-mode results NativeRawHighlights_Test compares RawHighlights with, and every
# other job tools/raw/make-reference.py lists. The jobs for modules that exist only on darktable
# master (contrast & texture) render with a second image at DARKTABLE_MASTER_REF; each run builds
# and uses the images its selected jobs need.
#
# Usage: tools/raw/make-reference.sh [--jobs N] [--keep-image] [--only TEXT] [--dry-run]
#
# The camera raw fixtures must be checked out (git lfs pull). The image is removed afterwards
# unless --keep-image is given; a later run with the image still present skips the build.
# --only renders just the jobs whose output name contains TEXT. --dry-run needs neither Docker
# nor the LFS files: it lists the jobs and writes their XMPs to build/raw-reference/xmp. A real
# run keeps make-reference.py's output, darktable's logged lines included, in
# build/raw-reference/make-reference.log.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
JOBS=4
KEEP_IMAGE=0
DRY_RUN=0
ONLY=()
while [ $# -gt 0 ]; do
    case "$1" in
        --jobs) JOBS="$2"; shift 2 ;;
        --keep-image) KEEP_IMAGE=1; shift ;;
        --only) ONLY=(--only "$2"); shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        *) echo "usage: $0 [--jobs N] [--keep-image] [--only TEXT] [--dry-run]" >&2; exit 2 ;;
    esac
done

FIXTURES="${ROOT}/Tests/fixtures/raw"
OUT="${FIXTURES}/reference"
LOG_DIR="${ROOT}/build/raw-reference"
mkdir -p "${LOG_DIR}"
pin() {
    sed -n "s/^$1=\"\(.*\)\"$/\1/p" "${ROOT}/tools/ci/local/fetch-assets.sh"
}
DARKTABLE_REPO="$(pin DARKTABLE_REPO)"
DARKTABLE_REF="$(pin DARKTABLE_REF)"
DARKTABLE_MASTER_REF="$(pin DARKTABLE_MASTER_REF)"
if [ -z "${DARKTABLE_REPO}" ] || [ -z "${DARKTABLE_REF}" ] || [ -z "${DARKTABLE_MASTER_REF}" ]; then
    echo "DARKTABLE_REPO, DARKTABLE_REF or DARKTABLE_MASTER_REF not found in tools/ci/local/fetch-assets.sh" >&2
    exit 1
fi
export DARKTABLE_REF DARKTABLE_MASTER_REF

if [ "${DRY_RUN}" = 1 ]; then
    exec python3 -I "${ROOT}/tools/raw/make-reference.py" --dry-run "${ONLY[@]}" \
        "${FIXTURES}" "${LOG_DIR}/out" "${LOG_DIR}/xmp"
fi

# The revisions the selected jobs render at, release before master: a master job may read a
# release job's output, never the other way round.
REFS=()
for ref in release master; do
    if python3 -I "${ROOT}/tools/raw/make-reference.py" --dry-run "${ONLY[@]}" --ref "${ref}" \
        "${FIXTURES}" "${LOG_DIR}/out" "${LOG_DIR}/xmp-${ref}" | grep -q " <- "; then
        REFS+=("${ref}")
    fi
done
if [ "${#REFS[@]}" = 0 ]; then
    echo "no job matches" >&2
    exit 1
fi

for f in canon-eos-5d-mark-iii.cr2 fujifilm-x-pro2-14bit-uncompressed.raf canon-eos-5d-mark-iii-linear-rgb.dng; do
    # An LFS pointer is a few hundred bytes of text.
    if [ "$(stat -c %s "${FIXTURES}/${f}")" -lt 1000000 ]; then
        echo "${FIXTURES}/${f} is not checked out; run git lfs pull" >&2
        exit 1
    fi
done

mkdir -p "${OUT}"
: > "${LOG_DIR}/make-reference.log"
for ref in "${REFS[@]}"; do
    if [ "${ref}" = master ]; then
        COMMIT="${DARKTABLE_MASTER_REF}"
    else
        COMMIT="${DARKTABLE_REF}"
    fi
    IMAGE="natron-darktable-ref:${COMMIT:0:12}"
    if ! docker image inspect "${IMAGE}" > /dev/null 2>&1; then
        CONTEXT="$(mktemp -d)"
        docker build -f "${ROOT}/tools/raw/darktable-ref.Dockerfile" \
            --build-arg DARKTABLE_REPO="${DARKTABLE_REPO}" \
            --build-arg DARKTABLE_REF="${COMMIT}" \
            --build-arg JOBS="${JOBS}" \
            -t "${IMAGE}" "${CONTEXT}"
        rmdir "${CONTEXT}"
    fi

    docker run --rm --user "$(id -u):$(id -g)" \
        -e HOME=/tmp -e DARKTABLE_REF="${DARKTABLE_REF:0:12}" -e DARKTABLE_MASTER_REF="${DARKTABLE_MASTER_REF:0:12}" \
        -v "${ROOT}/tools/raw:/tools:ro" \
        -v "${FIXTURES}:/fixtures:ro" \
        -v "${OUT}:/out" \
        --tmpfs /scratch:exec,size=3g \
        "${IMAGE}" \
        python3 /tools/make-reference.py "${ONLY[@]}" --ref "${ref}" /fixtures /out /scratch \
        | tee -a "${LOG_DIR}/make-reference.log"

    if [ "${KEEP_IMAGE}" = 0 ]; then
        docker image rm "${IMAGE}" > /dev/null
    fi
done
ls -l "${OUT}"
