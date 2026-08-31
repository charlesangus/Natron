#!/usr/bin/env bash
#
# One-time (per-cache) fetch of the two external assets CI re-downloads on
# every run: the OpenColorIO-Configs tarball and the openfx-io testing build.
#
# URLs and OCIO_CONFIG_VERSION here MUST match the "Download OpenColorIO-Configs"
# and "Download Plugins" steps in .github/workflows/ci.yml. Keep them in sync.
#
# Result (idempotent, safe to re-run):
#   build/assets/OpenColorIO-Configs/
#   build/assets/Plugins/
#
# A second run does no network work if both targets already look complete.
#
# This script has no CMake/ninja/gcc work to do -- only network fetches and
# unpacking -- but it still runs INSIDE the dev container, like
# build.sh/test.sh, re-execing itself through devshell.sh exactly once when
# invoked from the host. The point is that the documented local loop
# (fetch-assets -> build -> test) needs nothing on the host but Docker: the
# fetches below shell out to wget/tar/unzip, which the aswf/ci-baseqt image
# guarantees and an arbitrary developer host does not (unzip in particular
# is frequently absent). See build.sh's big comment block for the full
# reasoning behind in_container()'s two checks. Under CI this is a no-op:
# CI=true, so the re-exec is skipped and the script runs directly in the
# job's own container.

set -euo pipefail

# Resolve repo root from this script's own location, not $PWD.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." >/dev/null 2>&1 && pwd)"

# Returns success if we're already running inside a dev/CI container -- see
# build.sh's identical helper for why each of these checks exists.
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
    echo "== fetch-assets.sh: entering dev container via devshell.sh =="
    exec "${SCRIPT_DIR}/devshell.sh" "${REPO_ROOT}/tools/ci/local/fetch-assets.sh" "$@"
fi

# --- from here on, we are inside the dev container (or CI's container) -----

ASSETS_DIR="${REPO_ROOT}/build/assets"
OCIO_CONFIG_VERSION="${OCIO_CONFIG_VERSION:-2.5}"

mkdir -p "${ASSETS_DIR}"

echo "== fetch-assets.sh =="
echo "Repo root:   ${REPO_ROOT}"
echo "Assets dir:  ${ASSETS_DIR}"
echo "OCIO_CONFIG_VERSION=${OCIO_CONFIG_VERSION}"

# ---------------------------------------------------------------------------
# OpenColorIO-Configs
# ---------------------------------------------------------------------------
OCIO_TARGET="${ASSETS_DIR}/OpenColorIO-Configs"
OCIO_MARKER="${OCIO_TARGET}/blender/config.ocio"

if [ -e "${OCIO_MARKER}" ]; then
    echo "[OpenColorIO-Configs] already present and looks complete (${OCIO_MARKER}) -- skipping."
else
    echo "[OpenColorIO-Configs] fetching v${OCIO_CONFIG_VERSION}..."

    TMP_DIR="$(mktemp -d "${ASSETS_DIR}/.ocio-fetch.XXXXXX")"
    trap 'rm -rf "${TMP_DIR}"' EXIT

    TARBALL="${TMP_DIR}/Natron-v${OCIO_CONFIG_VERSION}.tar.gz"
    wget -O "${TARBALL}" \
        "https://github.com/NatronGitHub/OpenColorIO-Configs/archive/Natron-v${OCIO_CONFIG_VERSION}.tar.gz"
    tar xzf "${TARBALL}" -C "${TMP_DIR}"

    UNPACKED="${TMP_DIR}/OpenColorIO-Configs-Natron-v${OCIO_CONFIG_VERSION}"
    if [ ! -d "${UNPACKED}" ]; then
        echo "[OpenColorIO-Configs] ERROR: expected directory not found after unpack: ${UNPACKED}" >&2
        exit 1
    fi

    rm -rf "${OCIO_TARGET}"
    mv "${UNPACKED}" "${OCIO_TARGET}"

    rm -rf "${TMP_DIR}"
    trap - EXIT

    echo "[OpenColorIO-Configs] done -> ${OCIO_TARGET}"
fi

# ---------------------------------------------------------------------------
# Plugins (openfx-io testing build)
# ---------------------------------------------------------------------------
PLUGINS_TARGET="${ASSETS_DIR}/Plugins"

if [ -d "${PLUGINS_TARGET}" ] && [ -n "$(ls -A "${PLUGINS_TARGET}" 2>/dev/null)" ]; then
    echo "[Plugins] already present and non-empty (${PLUGINS_TARGET}) -- skipping."
else
    echo "[Plugins] fetching openfx-io testing build..."

    TMP_DIR="$(mktemp -d "${ASSETS_DIR}/.plugins-fetch.XXXXXX")"
    trap 'rm -rf "${TMP_DIR}"' EXIT

    ZIP="${TMP_DIR}/openfx-io-build-ubuntu_22-testing.zip"
    wget -O "${ZIP}" \
        "https://github.com/NatronGitHub/openfx-io/releases/download/natron_testing/openfx-io-build-ubuntu_22-testing.zip"

    UNPACK_DIR="${TMP_DIR}/unpacked"
    mkdir -p "${UNPACK_DIR}"
    unzip -q "${ZIP}" -d "${UNPACK_DIR}"

    rm -rf "${PLUGINS_TARGET}"
    mv "${UNPACK_DIR}" "${PLUGINS_TARGET}"

    rm -rf "${TMP_DIR}"
    trap - EXIT

    echo "[Plugins] done -> ${PLUGINS_TARGET}"
fi

echo "== fetch-assets.sh: all assets present =="
