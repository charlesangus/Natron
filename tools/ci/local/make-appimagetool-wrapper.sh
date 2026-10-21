#!/usr/bin/env bash
#
# Create the local appimagetool wrapper that package.sh puts ahead of PATH
# when building the AppImage.
#
# Usage:
#   tools/ci/local/make-appimagetool-wrapper.sh <dir> [<real-appimagetool>]
#
# <dir> receives two files: appimagetool (the wrapper) and appimagetool.real
# (a copy of the real tool). The real tool is the path given, else the first
# appimagetool found on PATH, else the appimagetool.real already sitting in the
# main checkout's build/appimagetool-wrapper/. tools/release/make-appimage.sh
# downloads into a temporary directory, so there is no persistent copy to
# reuse other than those.

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: make-appimagetool-wrapper.sh <dir> [<real-appimagetool>]" >&2
    exit 1
fi

DEST_DIR="$1"
REAL_TOOL="${2:-}"

if [[ -z "${REAL_TOOL}" ]]; then
    if command -v appimagetool >/dev/null 2>&1; then
        REAL_TOOL="$(command -v appimagetool)"
    else
        common_dir="$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"
        if [[ -n "${common_dir}" ]]; then
            candidate="$(dirname "${common_dir}")/build/appimagetool-wrapper/appimagetool.real"
            if [[ -f "${candidate}" ]]; then
                REAL_TOOL="${candidate}"
            fi
        fi
    fi
fi

if [[ -z "${REAL_TOOL}" || ! -f "${REAL_TOOL}" ]]; then
    echo "make-appimagetool-wrapper.sh: ERROR: no real appimagetool found." >&2
    echo "Pass its path as the second argument." >&2
    exit 1
fi

mkdir -p "${DEST_DIR}"

# A wrapper resolves through its own symlinks, so a PATH hit that is itself a
# wrapper must be copied from its target, not from the script.
REAL_TOOL="$(readlink -f "${REAL_TOOL}")"
if [[ "${REAL_TOOL}" != "$(readlink -f "${DEST_DIR}/appimagetool.real" 2>/dev/null || true)" ]]; then
    cp -f "${REAL_TOOL}" "${DEST_DIR}/appimagetool.real"
fi
chmod +x "${DEST_DIR}/appimagetool.real"

cat >"${DEST_DIR}/appimagetool" <<'WRAPPER'
#!/usr/bin/env bash
# Local-only wrapper (never committed, lives under gitignored build/) so
# make-appimage.sh's `command -v appimagetool` finds this instead of
# downloading the upstream tool, letting us pass -n/--no-appstream because
# this sandbox has no route to upload.wikimedia.org for the appdata.xml
# screenshot URL check, and no FUSE so it must self-extract-and-run.
export APPIMAGE_EXTRACT_AND_RUN=1
exec "$(dirname "$(readlink -f "$0")")/appimagetool.real" -n "$@"
WRAPPER
chmod +x "${DEST_DIR}/appimagetool"

echo "make-appimagetool-wrapper.sh: wrapper written to ${DEST_DIR}"
