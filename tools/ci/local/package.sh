#!/usr/bin/env bash
#
# Package an already-built tree (build/debug or build/release, see
# build.sh) into a relocatable tarball and AppImage, landing both under
# <build-dir>/artifacts/ -- the local-dev equivalent of release.yml's
# "Stage bundle" / "Create tarball" / "Create AppImage" steps, which only
# run on a tag push and drop their output in /tmp/artifacts instead of the
# build tree.
#
# Usage:
#   tools/ci/local/package.sh [debug|release]
#
# Like build.sh/test.sh, this is meant to be run from the host, from the
# repo root, with no prior devshell.sh shell required -- it re-execs itself
# through devshell.sh exactly once. Detection uses the same in_container()
# logic (NATRON_IN_CONTAINER=1, exported by devshell.sh's `docker run`,
# checked first; a case-insensitive CI check as fallback) -- see build.sh's
# comment for the full reasoning.
#
# appimagetool needs a route to upload.wikimedia.org to validate the
# AppData screenshot URL; some sandboxes have no such route. If
# build/appimagetool-wrapper/ exists (a local, gitignored wrapper -- see its
# own header -- that passes appimagetool -n/--no-appstream), it's put ahead
# of PATH so make-appimage.sh finds it instead of downloading the real tool
# -- a no-op everywhere that wrapper doesn't exist, e.g. CI.

set -euo pipefail

BUILD_TYPE="release"

for arg in "$@"; do
    case "${arg}" in
        debug|release)
            BUILD_TYPE="${arg}"
            ;;
        *)
            echo "package.sh: unknown argument '${arg}'" >&2
            echo "usage: package.sh [debug|release]" >&2
            exit 1
            ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." >/dev/null 2>&1 && pwd)"

in_container() {
    if [[ "${NATRON_IN_CONTAINER:-}" == "1" ]]; then
        return 0
    fi
    local ci_lower
    ci_lower="$(printf '%s' "${CI:-}" | tr '[:upper:]' '[:lower:]')"
    if [[ "${ci_lower}" == "true" ]]; then
        return 0
    fi
    return 1
}

if ! in_container; then
    echo "== package.sh: entering dev container via devshell.sh =="
    exec "${SCRIPT_DIR}/devshell.sh" "${REPO_ROOT}/tools/ci/local/package.sh" "$@"
fi

# --- from here on, we are inside the dev container ---------------------------

BUILD_DIR="${REPO_ROOT}/build/${BUILD_TYPE}"

if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    echo "package.sh: ERROR: ${BUILD_DIR} has not been built yet." >&2
    echo "Run 'tools/ci/local/build.sh ${BUILD_TYPE}' first." >&2
    exit 1
fi

STAGE_DIR="$(mktemp -d)"
trap 'rm -rf "${STAGE_DIR}"' EXIT

ARTIFACTS_DIR="${BUILD_DIR}/artifacts"
mkdir -p "${ARTIFACTS_DIR}"

APPIMAGETOOL_WRAPPER="${REPO_ROOT}/build/appimagetool-wrapper"
if [[ -d "${APPIMAGETOOL_WRAPPER}" ]]; then
    PATH="${APPIMAGETOOL_WRAPPER}:${PATH}"
fi

echo "== package.sh: staging ${BUILD_DIR} =="
"${SCRIPT_DIR}/../../release/stage-bundle.sh" "${BUILD_DIR}" "${STAGE_DIR}"

echo "== package.sh: building tarball =="
"${SCRIPT_DIR}/../../release/make-tarball.sh" "${STAGE_DIR}" "${ARTIFACTS_DIR}"

echo "== package.sh: building AppImage =="
"${SCRIPT_DIR}/../../release/make-appimage.sh" "${STAGE_DIR}" "${ARTIFACTS_DIR}"

echo "== package.sh: artifacts ready in ${ARTIFACTS_DIR} =="
ls -la "${ARTIFACTS_DIR}"
