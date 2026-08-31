#!/usr/bin/env bash
#
# One-time (per-cache) fetch/setup of the things CI re-does on every run
# that this repo's tree doesn't already contain: the OpenColorIO-Configs
# tarball, the openfx-io testing build, and (see M2.P3.T1d) the `qtpy`
# Python package the smoke test imports.
#
# URLs and OCIO_CONFIG_VERSION here MUST match the "Download OpenColorIO-Configs"
# and "Download Plugins" steps in .github/workflows/ci.yml. Keep them in sync.
#
# Result (idempotent, safe to re-run):
#   build/assets/OpenColorIO-Configs/
#   build/assets/Plugins/
#   `qtpy` importable by the container's python3, from system
#   site-packages -- see the "Python test dependencies" section below for
#   why it has to be system-wide rather than a `--user` install.
#
# A second run does no network work if everything already looks complete.
#
# Unlike build.sh/test.sh, this script has no CMake/ninja/gcc work to do --
# only network fetches and a pip install -- but the pip install still needs
# to land on the SAME python3 that NatronRenderer's embedded interpreter
# will resolve `import qtpy` against, i.e. the dev container's python3, not
# whatever `python3` happens to mean on the host. So, like build.sh/test.sh,
# this script re-execs itself through devshell.sh unless it's already
# running inside a dev/CI container. See build.sh's big comment block for
# the full reasoning behind in_container()'s two checks.

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

# ---------------------------------------------------------------------------
# Python test dependencies (qtpy)
# ---------------------------------------------------------------------------
# tools/ci/smoke_test.py's first assertion is `import qtpy` /
# `qtpy.API_NAME == 'PySide6'` (see M2.P3.T1a), but nothing installs qtpy --
# not ci.yml, not the aswf/ci-baseqt base image. This installs it here so
# the smoke test can actually run, in CI and locally, from one change.
#
# This MUST land in system site-packages, not a `--user`/`--target` install:
# NatronRenderer's embedded Python interpreter explicitly disables user-site
# (see AppManager::initPython() / Global/PythonUtils.cpp's
# `PYTHONNOUSERSITE=1`, set there so Natron's own bundled packages can't be
# shadowed by whatever happens to be in a user's site-packages) and does not
# add any extra site-packages directory of its own when running against a
# system (non-bundled) Python, as this build does -- so anything installed
# with `--user` resolves fine from a bare `python3 -c "import qtpy"` but is
# invisible to `import qtpy` from *inside* NatronRenderer. Confirmed
# empirically: `--user` passed `test.sh smoke`'s "python3 -c" spot-check
# but `test.sh smoke` itself still hit `ModuleNotFoundError: No module
# named 'qtpy'` (see M2.P3.T1d's report) -- this is exactly the
# root-vs-uid-1000 trap the task called out, just one level less obvious
# than a plain permissions error.
#
# System site-packages (/usr/local/lib/python3.13/site-packages) is
# root-owned 755 (verified), so it needs root to write to it:
#   - CI runs this script as root directly (ci.yml's job container has no
#     `user:` override) -- a plain, unprivileged-looking `pip3 install`
#     here already IS a root install in that context.
#   - The local dev container runs everything else as uid 1000 (see
#     devshell.sh's `docker run --user "${UID_GID}"`), which cannot write
#     there. This script cannot self-escalate from inside that container
#     (no docker-in-docker/sudo available), so devshell.sh instead installs
#     qtpy as root, once, at container creation, via `docker exec -u 0:0`
#     (which can run as any uid regardless of the container's default user)
#     -- see its "one-time root-level install of qtpy" step. By the time
#     this script runs locally (always re-exec'd into that same
#     already-provisioned container, see the top of this file), qtpy
#     should already be there, so the branch below only needs to verify
#     that and give an actionable error (recreate the container) if not --
#     it does not attempt the install itself for the non-root case.
#
# qtpy is a pure-Python wheel (py3-none-any) -- no ABI/platform concerns
# from installing it independently of the image's own package set.
#
# Pin: a floor + major-version ceiling (>=2.0,<3), not an exact version.
# 2.0 is the floor for QT_API=pyside6 support at all; capping below 3
# avoids an unvetted future major version silently changing qtpy's API
# resolution behavior, but an exact pin isn't warranted for a lightweight
# compatibility shim that isn't part of the shipped product.
#
# The pin itself is single-sourced via the QTPY_PIN env var (ci.yml sets it
# for CI; devshell.sh sets it -- from the same default -- for the container
# it creates, and this script inherits it since it always runs inside that
# container). The literal fallback below only matters if this script is
# ever run some other way; keep it matching ci.yml's QTPY_PIN if it changes.
QTPY_PIN="${QTPY_PIN:-qtpy>=2.0,<3}"
if python3 -c "import qtpy" >/dev/null 2>&1; then
    echo "[qtpy] already importable -- skipping (no network work)."
elif [[ "$(id -u)" -eq 0 ]]; then
    echo "[qtpy] not importable yet, running as root -- installing ${QTPY_PIN} into system site-packages..."
    python3 -m pip install --root-user-action=ignore "${QTPY_PIN}"
else
    cat >&2 <<'EOF'
[qtpy] ERROR: qtpy is not importable, and this script is not running as
root, so it cannot install into system site-packages here (see the
comment above this check: NatronRenderer's embedded Python disables
user-site, so a --user install would not be visible to it anyway).

devshell.sh installs qtpy as root, once, at container creation -- this
container was most likely created before that step existed. Recreate it
to pick it up (caches are kept, see tools/ci/local/README.md):

    tools/ci/local/devshell.sh --recreate
EOF
    exit 1
fi
python3 -c "import qtpy; print('[qtpy] OK: import qtpy -> API_NAME=%r version=%s (%s)' % (qtpy.API_NAME, qtpy.__version__, qtpy.__file__))"

echo "== fetch-assets.sh: all assets present =="
