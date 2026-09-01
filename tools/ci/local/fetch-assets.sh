#!/usr/bin/env bash
#
# Prepare the two external assets the test suite needs: the
# OpenColorIO-Configs tarball, and the openfx-io OFX plugin bundle that
# Tests/BaseTest.cpp loads through OFX_PLUGIN_PATH.
#
# Result (idempotent, safe to re-run):
#   build/assets/OpenColorIO-Configs/
#   build/assets/Plugins/IO.ofx.bundle/
#
# A second run does no network or compile work if both targets already look
# complete and the plugin stamp matches the pins below.
#
# --- Why the plugins are BUILT here, not downloaded -----------------------
#
# This used to `wget` a prebuilt openfx-io bundle
# (openfx-io-build-ubuntu_22-testing.zip). That bundle cannot load in the
# CY2027 container and never will: it is an Ubuntu 22 build that needs
# GLIBCXX_3.4.30 (Rocky 9 ships at most 3.4.29) and links an entire ABI
# generation behind us -- libOpenColorIO.so.1, libOpenImageIO.so.2.2,
# libIlmImf-2_5.so.25, libavformat.so.58. Loading it fails with:
#
#   couldn't open library .../IO.ofx because /lib64/libstdc++.so.6:
#   version `GLIBCXX_3.4.30' not found
#
# There is no EL9 build published upstream. The answer is to build the
# plugin against the container's own libraries, which the aswf/ci-vfxall
# image makes possible -- it ships OpenColorIO, OpenImageIO, OpenEXR,
# OpenFX and LibRaw, where the older ci-baseqt image shipped none of them.
# See PLAN/DECISIONS/2026-08-31-restore-vendored-ofx-plugin-tests.md.
#
# SeExpr is built too, because ASWF ships none at any VFX Platform year and
# openfx-io's SeNoise plugin -- one of the three IDs BaseTest asserts --
# needs it. It is linked STATICALLY on purpose: that makes the resulting
# IO.ofx self-contained, so neither this script, test.sh, nor CI has to
# manage an LD_LIBRARY_PATH for it. Verified: the built IO.ofx has no
# libSeExpr entry in DT_NEEDED.
#
# This script therefore DOES do compile work now, which is the main reason
# it (like build.sh/test.sh) runs INSIDE the dev container, re-execing
# itself through devshell.sh exactly once when invoked from the host: the
# documented local loop (fetch-assets -> build -> test) needs nothing on the
# host but Docker. See build.sh's big comment block for the full reasoning
# behind in_container()'s two checks. Under CI this is a no-op: CI=true, so
# the re-exec is skipped and the script runs directly in the job's own
# container.

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
# Plugins (openfx-io, built from source against this container's libraries)
# ---------------------------------------------------------------------------
#
# Both pins are exact commit SHAs, not branches or tags: this bundle is a
# test fixture, and a fixture that silently changes under you turns an
# unrelated upstream commit into a mystery CI failure on your PR. Bump them
# deliberately, and re-run this script (it rebuilds when the stamp below no
# longer matches).
#
# OPENFX_IO_REF: charlesangus/openfx-io -- our fork, four commits ahead of
# NatronGitHub/openfx-io and zero behind. Fork-and-fix is the standing
# pattern for small changes to NatronGitHub repos -- see
# PLAN/DECISIONS/2026-08-31-fork-and-fix-natrongithub-repos.md. Two deltas:
#
# 1. A CMakeLists.txt fix (SEEXPR2_INCLUDES/SEEXPR2_LIBRARIES ->
#    SEEXPR2_INCLUDE_DIR/SEEXPR2_LIBRARY): upstream reads variable names its
#    own FindSeExpr2.cmake never sets, so the SeExpr sources compiled but
#    IO.ofx was never linked against libSeExpr, and the bundle failed to load
#    with `undefined symbol: _ZTI11SeExprFuncX`. Undefined symbols in a shared
#    library don't fail a link by default, which is why this went unnoticed
#    upstream.
#
# 2. Colorspace resolution in IOSupport/GenericOCIO.cpp, which is what lets
#    the bundle work against Natron's default OCIO config
#    (ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5). Reader and writer
#    colorspace parameter defaults are computed at describe time and were
#    never checked against the config actually loaded, so on any config that
#    defines no `default` role -- every ACES config -- a fresh project failed
#    to render with `Color space 'default' could not be found.` The fix
#    guards canonicalizeColorSpace()'s -1 "not found" sentinel (which
#    otherwise compares equal to the -1 of an undefined role), falls back to
#    scene_linear rather than to colorspace 0 (display-referred in the ACES
#    configs), and teaches the fallback chains the ACES spellings
#    `sRGB - Display` and `Camera Rec.709`. Against the older tarball configs
#    this is a no-op: renders are byte-identical either side of it.
#
# The -1 sentinel guard is the first of the two commits and is deliberately
# self-contained, so it can be offered upstream on its own.
#
# Verified to build clean against the image's OIIO 3.1.16.0 / OCIO 2.5.2 /
# OpenEXR 3.4.15 -- openfx-io carries explicit `#if OIIO_VERSION >= 30000`
# support, so OIIO 3 is a supported configuration upstream, not something we
# are forcing.
#
# SEEXPR_REF: wdas/SeExpr, branch v1-2.11. NOT the v2/v3 line: openfx-io's
# SeNoise.cpp includes <SeExprBuiltins.h>/<SeNoise.h> and shims
# `#define SeExpr2 SeExpr`, i.e. it targets the v1-2.11 header layout.
# openfx-io's own CI pins the same branch. Not forked -- wdas/SeExpr is not
# a NatronGitHub repo and we carry no changes to it.
OPENFX_IO_REPO="https://github.com/charlesangus/openfx-io.git"
OPENFX_IO_REF="40764b207277d42c8c6a9060f6fc40c2eab80b7f"
SEEXPR_REPO="https://github.com/wdas/SeExpr.git"
SEEXPR_REF="a5f02bb03199630759b0b94a64f37ce56c08675a"

PLUGINS_TARGET="${ASSETS_DIR}/Plugins"
PLUGINS_SRC_DIR="${ASSETS_DIR}/plugin-src"
PLUGINS_STAMP="${PLUGINS_TARGET}/.natron-plugin-pins"
PLUGINS_WANT="openfx-io=${OPENFX_IO_REF} seexpr=${SEEXPR_REF}"

if [ -f "${PLUGINS_STAMP}" ] && [ "$(cat "${PLUGINS_STAMP}")" = "${PLUGINS_WANT}" ]; then
    echo "[Plugins] already built at the pinned refs -- skipping."
    echo "[Plugins]   ${PLUGINS_WANT}"
else
    echo "[Plugins] building openfx-io from source..."
    echo "[Plugins]   ${PLUGINS_WANT}"

    mkdir -p "${PLUGINS_SRC_DIR}"

    # Fetch exactly one commit each. `git clone --depth 1` cannot take a raw
    # SHA on most servers, so init + fetch that SHA directly instead; it is
    # the cheapest way to land on an exact pin without cloning history.
    clone_at_ref() {
        local repo="$1" ref="$2" dest="$3"
        if [ -d "${dest}/.git" ] && [ "$(git -C "${dest}" rev-parse HEAD 2>/dev/null)" = "${ref}" ]; then
            echo "[Plugins]   ${dest##*/}: already at ${ref}"
            return 0
        fi
        rm -rf "${dest}"
        mkdir -p "${dest}"
        git -C "${dest}" init -q
        git -C "${dest}" remote add origin "${repo}"
        git -C "${dest}" fetch -q --depth 1 origin "${ref}"
        git -C "${dest}" checkout -q FETCH_HEAD
        git -C "${dest}" submodule update -q --init --recursive --depth 1
    }

    clone_at_ref "${OPENFX_IO_REPO}" "${OPENFX_IO_REF}" "${PLUGINS_SRC_DIR}/openfx-io"
    clone_at_ref "${SEEXPR_REPO}"    "${SEEXPR_REF}"    "${PLUGINS_SRC_DIR}/SeExpr"

    # --- SeExpr ------------------------------------------------------------
    # The sed is openfx-io's own CI recipe: drop the editor/demos/tests/doc
    # subdirectories, which pull in a Qt4/Qt5 dependency this image cannot
    # satisfy (its qmake is Qt 6.8.3, which SeExpr's find_package rejects as
    # "unsuitable") and which we have no use for.
    #
    # POSITION_INDEPENDENT_CODE is what makes the static archive linkable
    # into IO.ofx (a shared object); without it the link fails on
    # relocations. CMAKE_POLICY_VERSION_MINIMUM is needed because both
    # projects declare cmake_minimum_required(VERSION 3.1) and the image
    # ships CMake 4.x, which refuses <3.5 compatibility outright.
    SEEXPR_PREFIX="${PLUGINS_SRC_DIR}/SeExpr-install"
    if [ ! -f "${SEEXPR_PREFIX}/lib/libSeExpr.a" ]; then
        echo "[Plugins] building SeExpr (static)..."
        sed -i -e "/SeExprEditor/d" -e "/demos/d" -e "/tests/d" -e "/doc/d" \
            "${PLUGINS_SRC_DIR}/SeExpr/CMakeLists.txt"
        cmake -S "${PLUGINS_SRC_DIR}/SeExpr" -B "${PLUGINS_SRC_DIR}/SeExpr/build" \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="${SEEXPR_PREFIX}" \
            -DCMAKE_INSTALL_LIBDIR=lib \
            -DCMAKE_CXX_STANDARD=11 \
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
            -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
            > /dev/null
        cmake --build "${PLUGINS_SRC_DIR}/SeExpr/build" -j "$(nproc)" > /dev/null
        cmake --install "${PLUGINS_SRC_DIR}/SeExpr/build" > /dev/null
    fi

    # --- openfx-io ---------------------------------------------------------
    # SEEXPR2_LIBRARY/SEEXPR2_INCLUDE_DIR tell FindSeExpr2.cmake where our
    # locally-built SeExpr landed; it searches default paths only, and our
    # prefix is deliberately not one. The plural spellings that used to be
    # passed here too are gone -- the fork's CMakeLists.txt now reads these
    # singular names, which is the whole point of pinning the fork.
    #
    # FFmpeg is deliberately absent (ASWF ships none), so ReadFFmpeg and
    # WriteFFmpeg are not in the bundle. Nothing in the test suite needs
    # them; if that changes, ffmpeg has to come from somewhere first.
    echo "[Plugins] building openfx-io..."
    IO_BUILD="${PLUGINS_SRC_DIR}/openfx-io/build"
    rm -rf "${IO_BUILD}"
    cmake -S "${PLUGINS_SRC_DIR}/openfx-io" -B "${IO_BUILD}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH=/usr/local \
        -DSEEXPR2_LIBRARY="${SEEXPR_PREFIX}/lib/libSeExpr.a" \
        -DSEEXPR2_INCLUDE_DIR="${SEEXPR_PREFIX}/include" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    cmake --build "${IO_BUILD}" -j "$(nproc)"

    # openfx-io's install target already emits the OFX bundle layout
    # (IO.ofx.bundle/Contents/{Linux-x86-64,Resources,Info.plist}), which is
    # exactly what OFX_PLUGIN_PATH expects -- no hand-assembly needed.
    rm -rf "${PLUGINS_TARGET}"
    mkdir -p "${PLUGINS_TARGET}"
    cmake --install "${IO_BUILD}" --prefix "${PLUGINS_TARGET}" > /dev/null

    IO_OFX="${PLUGINS_TARGET}/IO.ofx.bundle/Contents/Linux-x86-64/IO.ofx"
    if [ ! -f "${IO_OFX}" ]; then
        echo "[Plugins] ERROR: build produced no ${IO_OFX}" >&2
        exit 1
    fi

    # Fail loudly here rather than let BaseTest fail with the far less
    # informative "Couldn't find a plugin attached to the ID ...". These are
    # exactly the three IDs Tests/BaseTest.cpp asserts.
    #
    # Deliberately NOT `strings ... | grep -q`: this script runs under
    # `set -o pipefail`, and `grep -q` exits the moment it matches, which
    # kills the writer upstream of it with SIGPIPE (exit 141). pipefail then
    # reports the whole pipeline as failed *because* the id was found -- a
    # false negative that reads exactly like a real build failure. Dumping
    # the symbols to a file once and grepping the file keeps `grep -q` out
    # of a pipeline entirely.
    IO_SYMBOLS="${IO_BUILD}/io-symbols.txt"
    strings "${IO_OFX}" > "${IO_SYMBOLS}"
    for id in fr.inria.openfx.ReadOIIO fr.inria.openfx.WriteOIIO net.sf.openfx.SeNoise; do
        if ! grep -Fxq -- "${id}" "${IO_SYMBOLS}"; then
            echo "[Plugins] ERROR: built bundle does not export ${id}" >&2
            exit 1
        fi
    done

    echo "${PLUGINS_WANT}" > "${PLUGINS_STAMP}"
    echo "[Plugins] done -> ${PLUGINS_TARGET}"
fi

echo "== fetch-assets.sh: all assets present =="
