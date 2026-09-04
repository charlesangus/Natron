#!/usr/bin/env bash
#
# Prepare the external assets the test suite and Natron's shipped plugin set
# need: the OpenColorIO-Configs tarball, the openfx-io OFX plugin bundle that
# Tests/BaseTest.cpp loads through OFX_PLUGIN_PATH, and the openfx-misc/
# openfx-arena OFX plugin bundles that ship with Natron but that no test
# currently loads -- they are built here so CI proves they still build
# against the pinned toolchain, the same way the rest of Natron is proven to
# build.
#
# Result (idempotent, safe to re-run):
#   build/assets/OpenColorIO-Configs/
#   build/assets/Plugins/IO.ofx.bundle/
#   build/assets/Plugins/Misc.ofx.bundle/
#   build/assets/Plugins/CImg.ofx.bundle/
#   build/assets/Plugins/Arena.ofx.bundle/
#   build/assets/plugin-src/deps-install/  (lcms2, libzip, ImageMagick --
#                                           for openfx-arena)
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
# openfx-misc is built the same way and needs no fork: unlike openfx-io it
# has no dependency on OIIO/OCIO/SeExpr and no CMakeLists.txt bug, so upstream
# builds clean against this container as-is. It does have one prerequisite
# its own CMakeLists.txt doesn't handle -- see the comment above the
# openfx-misc build step below.
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
# Plugins (openfx-io and openfx-misc, built from source against this
# container's libraries)
# ---------------------------------------------------------------------------
#
# All pins are exact commit SHAs, not branches or tags: this bundle is a
# test fixture (openfx-io) or a build-health check (openfx-misc), and either
# one silently changing under you turns an unrelated upstream commit into a
# mystery CI failure on your PR. Bump them deliberately, and re-run this
# script (it rebuilds when the stamp below no longer matches).
#
# OPENFX_IO_REF: charlesangus/openfx-io -- our fork, five commits ahead of
# NatronGitHub/openfx-io and zero behind. Fork-and-fix is the standing
# pattern for small changes to NatronGitHub repos -- see
# PLAN/DECISIONS/2026-08-31-fork-and-fix-natrongithub-repos.md. Three deltas:
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
# 3. timeOffset consistency in IOSupport/GenericReader.cpp
#    (charlesangus/openfx-io#2). GenericReaderPlugin keeps one time mapping
#    in two params -- getTimeDomain() reads startingTime, getSequenceTime()
#    decodes with `t - timeOffset` -- so they must satisfy
#    `timeOffset == startingTime - firstFrame`. Every branch of
#    changedParam() maintained that except kParamOriginalFrameRange, which
#    resets firstFrame/lastFrame/startingTime to a newly chosen file's range
#    and left timeOffset holding the old file's value. A reader whose
#    startingTime had been moved off its first frame then advertised a frame
#    range it could not decode: every time in it mapped outside the sequence
#    domain and onMissingFrame's hold collapsed the whole range onto one
#    frame. That is the defect Tests/fixtures/read-time-offset.ntp and
#    smoke_test.py's check_reader_cli_time_offset_regression pin down.
#    Readers already at timeOffset 0 are unaffected -- renders are
#    pixel-identical either side of it.
#
# The -1 sentinel guard is the first of delta 2's two commits and is
# deliberately self-contained, so it can be offered upstream on its own; so
# is delta 3, which is one commit and touches nothing else.
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
OPENFX_IO_REF="020d898f9bc92191a6fc1cb5a69bbd3c641ed23d"
SEEXPR_REPO="https://github.com/wdas/SeExpr.git"
SEEXPR_REF="a5f02bb03199630759b0b94a64f37ce56c08675a"

# OPENFX_MISC_REF: NatronGitHub/openfx-misc, upstream directly -- not forked.
# Unlike openfx-io, its CMakeLists.txt has no variable-name bug and nothing in
# it depends on OIIO/OCIO/SeExpr, so it configures and links clean against
# this container with no source changes needed. (If that ever stops being
# true, fork it the same way -- see
# PLAN/DECISIONS/2026-08-31-fork-and-fix-natrongithub-repos.md -- rather than
# patching it from this script.)
OPENFX_MISC_REPO="https://github.com/NatronGitHub/openfx-misc.git"
OPENFX_MISC_REF="0abd46b5a8cbc98fa24579042129460d0aa87b8f"

# LCMS2_REF: mm2/Little-CMS at the lcms2.16 tag. Built from source even
# though the image already ships /usr/local/lib/liblcms2.so.2.0.19 with a
# header, because it ships no lcms2.pc -- and openfx-arena (a later plugin
# in this set) resolves it via pkg_search_module(LCMS2 REQUIRED lcms2).
# Hand-writing a .pc for a library this repo doesn't build would drift
# silently against the real image; building the pinned tag is cheaper to
# keep honest.
#
# LIBZIP_REF: nih-at/libzip at the v1.11.4 tag. Absent from the image
# entirely (no header, no library, no package) -- also needed by
# openfx-arena.
LCMS2_REPO="https://github.com/mm2/Little-CMS.git"
LCMS2_REF="453bafeb85b4ef96498866b7a8eadcc74dff9223"
LIBZIP_REPO="https://github.com/nih-at/libzip.git"
LIBZIP_REF="6f8a0cdd24a0dc6cce9dac4a7679da784ab124ea"

# IMAGEMAGICK_REF: ImageMagick/ImageMagick at the 7.1.1-6 tag, also needed by
# openfx-arena. Built Q32/HDRI: every Magick-based OFX plugin round-trips
# float buffers through Magick::FloatPixel, and a non-HDRI (integer) quantum
# would silently clip linear values on the way through. Flags are upstream
# Natron's own (tools/jenkins/include/scripts/pkg/imagemagick7.sh as of
# 576c2078^, before that tree was deleted), minus its CentOS-6 conditionals,
# plus two deltas this container forces -- see the comments on the build step
# below.
IMAGEMAGICK_REPO="https://github.com/ImageMagick/ImageMagick.git"
IMAGEMAGICK_REF="b2dd67b1681e23d0e0b9769d81bed23f05129e2a"

# OPENFX_ARENA_REF: charlesangus/openfx-arena -- our fork, adding
# WITH_SVG/WITH_PDF/WITH_CDR CMake options (all defaulting ON, so upstream
# behaviour is unchanged) that gate the three readers whose dependencies
# (librsvg, poppler-glib, libcdr/librevenge) this image does not ship. We
# turn them OFF below rather than carry the missing libraries.
OPENFX_ARENA_REPO="https://github.com/charlesangus/openfx-arena.git"
OPENFX_ARENA_REF="d29ac7f180e1ea5cca2b8c0492686638836b605c"

PLUGINS_TARGET="${ASSETS_DIR}/Plugins"
PLUGINS_SRC_DIR="${ASSETS_DIR}/plugin-src"
DEPS_PREFIX="${PLUGINS_SRC_DIR}/deps-install"
PLUGINS_STAMP="${PLUGINS_TARGET}/.natron-plugin-pins"
PLUGINS_WANT="openfx-io=${OPENFX_IO_REF} seexpr=${SEEXPR_REF} openfx-misc=${OPENFX_MISC_REF} lcms2=${LCMS2_REF} libzip=${LIBZIP_REF} imagemagick=${IMAGEMAGICK_REF} openfx-arena=${OPENFX_ARENA_REF}"

# Defined unconditionally (not just in the build branch below) so both the
# "already built -- skipping" and the "building..." paths can probe these
# bundles with verify_plugin_loads afterwards.
IO_OFX="${PLUGINS_TARGET}/IO.ofx.bundle/Contents/Linux-x86-64/IO.ofx"
MISC_OFX="${PLUGINS_TARGET}/Misc.ofx.bundle/Contents/Linux-x86-64/Misc.ofx"
CIMG_OFX="${PLUGINS_TARGET}/CImg.ofx.bundle/Contents/Linux-x86-64/CImg.ofx"
ARENA_OFX="${PLUGINS_TARGET}/Arena.ofx.bundle/Contents/Linux-x86-64/Arena.ofx"

if [ -f "${PLUGINS_STAMP}" ] && [ "$(cat "${PLUGINS_STAMP}")" = "${PLUGINS_WANT}" ]; then
    echo "[Plugins] already built at the pinned refs -- skipping."
    echo "[Plugins]   ${PLUGINS_WANT}"
else
    echo "[Plugins] building openfx-io and openfx-misc from source..."
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

    clone_at_ref "${OPENFX_IO_REPO}"   "${OPENFX_IO_REF}"   "${PLUGINS_SRC_DIR}/openfx-io"
    clone_at_ref "${SEEXPR_REPO}"      "${SEEXPR_REF}"      "${PLUGINS_SRC_DIR}/SeExpr"
    clone_at_ref "${OPENFX_MISC_REPO}" "${OPENFX_MISC_REF}" "${PLUGINS_SRC_DIR}/openfx-misc"
    clone_at_ref "${LCMS2_REPO}"       "${LCMS2_REF}"       "${PLUGINS_SRC_DIR}/Little-CMS"
    clone_at_ref "${LIBZIP_REPO}"      "${LIBZIP_REF}"      "${PLUGINS_SRC_DIR}/libzip"
    clone_at_ref "${IMAGEMAGICK_REPO}" "${IMAGEMAGICK_REF}" "${PLUGINS_SRC_DIR}/ImageMagick"
    clone_at_ref "${OPENFX_ARENA_REPO}" "${OPENFX_ARENA_REF}" "${PLUGINS_SRC_DIR}/openfx-arena"

    # --- lcms2 ---------------------------------------------------------
    # `configure` is checked into the tag (not generated at release time
    # only), so no autoreconf/autogen.sh step is needed here.
    # --disable-static: only the shared lib + .pc are needed downstream.
    if [ ! -f "${DEPS_PREFIX}/lib/pkgconfig/lcms2.pc" ]; then
        echo "[Plugins] building lcms2..."
        (
            cd "${PLUGINS_SRC_DIR}/Little-CMS"
            ./configure --prefix="${DEPS_PREFIX}" --disable-static > /dev/null
            make -j "$(nproc)" > /dev/null
            make install > /dev/null
        )
    fi

    # --- libzip ----------------------------------------------------------
    # All optional compression backends turned off: this bundle only needs
    # plain zip read/write, and each extra codec is another image library
    # this build would otherwise have to trust find_package() to locate
    # correctly.
    if [ ! -f "${DEPS_PREFIX}/lib/pkgconfig/libzip.pc" ]; then
        echo "[Plugins] building libzip..."
        LIBZIP_BUILD="${PLUGINS_SRC_DIR}/libzip/build"
        rm -rf "${LIBZIP_BUILD}"
        cmake -S "${PLUGINS_SRC_DIR}/libzip" -B "${LIBZIP_BUILD}" \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="${DEPS_PREFIX}" \
            -DCMAKE_INSTALL_LIBDIR=lib \
            -DBUILD_TOOLS=OFF \
            -DBUILD_REGRESS=OFF \
            -DBUILD_EXAMPLES=OFF \
            -DBUILD_DOC=OFF \
            -DENABLE_BZIP2=OFF \
            -DENABLE_LZMA=OFF \
            -DENABLE_ZSTD=OFF \
            > /dev/null
        cmake --build "${LIBZIP_BUILD}" -j "$(nproc)" > /dev/null
        cmake --install "${LIBZIP_BUILD}" > /dev/null
    fi

    # --- ImageMagick -----------------------------------------------------
    # PKG_CONFIG_PATH points at DEPS_PREFIX first so --with-lcms resolves the
    # lcms2 built above (the image's own liblcms2 ships no .pc, so without
    # this configure would not find lcms2 at all rather than finding the
    # wrong one).
    #
    # --without-jxl: this image's libjxl (0.12 under /usr/local) changed
    # JxlDecoderGetColorAsICCProfile's signature and ImageMagick's
    # coders/jxl.c does not compile against it.
    #
    # LDFLAGS/CPPFLAGS: the image carries FreeType 2.10.4 at /usr/lib64 and
    # 2.13.x at /usr/local; `pkg-config freetype2` resolves the former, but
    # /usr/local/lib/libharfbuzz.so.0 needs the latter's FT_Get_Paint*
    # symbols. Forcing the /usr/local headers and an rpath back to
    # /usr/local/lib is what makes the two agree. This FreeType split is an
    # image-level property, not specific to ImageMagick -- expect it to bite
    # any future font-touching dependency built in this container.
    if [ ! -f "${DEPS_PREFIX}/lib/pkgconfig/Magick++.pc" ]; then
        echo "[Plugins] building ImageMagick (Q32 HDRI)..."
        (
            cd "${PLUGINS_SRC_DIR}/ImageMagick"
            env PKG_CONFIG_PATH="${DEPS_PREFIX}/lib/pkgconfig:${PKG_CONFIG_PATH:-}" \
                CFLAGS="-DMAGICKCORE_EXCLUDE_DEPRECATED=1" \
                CXXFLAGS="-DMAGICKCORE_EXCLUDE_DEPRECATED=1" \
                CPPFLAGS="-I/usr/local/include/freetype2" \
                LDFLAGS="-L/usr/local/lib -Wl,-rpath,/usr/local/lib" \
                ./configure --prefix="${DEPS_PREFIX}" \
                --with-magick-plus-plus=yes --with-quantum-depth=32 --enable-hdri \
                --disable-static --enable-shared --without-modules \
                --enable-zero-configuration --without-x \
                --with-lcms --with-png --with-zlib --with-freetype --with-fontconfig \
                --with-libheif --without-jxl \
                --without-dps --without-djvu --without-fftw --without-fpx \
                --without-gslib --without-gvc --without-jbig --without-jpeg \
                --without-openjp2 --without-lqr --without-lzma --without-openexr \
                --without-pango --without-rsvg --without-tiff --without-webp \
                --without-xml --without-bzlib \
                > /dev/null
            make -j "$(nproc)" > /dev/null
            make install > /dev/null
        )
    fi

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

    # --- openfx-misc ---------------------------------------------------------
    # openfx-misc's CMakeLists.txt globs CImg/CImg.h and
    # CImg/Inpaint/inpaint.h as sources for the CImg.ofx target, but never
    # fetches them -- it assumes they're already there. Upstream's own
    # Makefile build (not the CMake build CI uses) has rules that curl both
    # files from dtschump/CImg and patch inpaint.h for abort support; CMake
    # has no equivalent, so a plain `cmake --build` links Misc.ofx (it doesn't
    # need CImg.h) and then fails partway through CImg.ofx with a "no such
    # file" on Inpaint/inpaint.h. We replicate upstream's two curl calls and
    # the patch, sourcing the CImg commit from openfx-misc's own
    # CImg/Makefile rather than pinning it separately here -- that keeps it
    # locked to whatever openfx-misc's own recipe expects at OPENFX_MISC_REF,
    # so it can never drift out of sync when that ref is bumped.
    echo "[Plugins] building openfx-misc..."
    MISC_SRC="${PLUGINS_SRC_DIR}/openfx-misc"
    MISC_BUILD="${MISC_SRC}/build"

    CIMG_VERSION="$(sed -n 's/^CIMGVERSION=//p' "${MISC_SRC}/CImg/Makefile")"
    if [ -z "${CIMG_VERSION}" ]; then
        echo "[Plugins] ERROR: could not read CIMGVERSION from ${MISC_SRC}/CImg/Makefile" >&2
        exit 1
    fi
    # -f (fail on HTTP error) matters here: without it, a 404 writes GitHub's
    # HTML error page to CImg.h/inpaint.h instead of failing the fetch, and
    # the build then dies confusingly deep in a C++ parse error instead of a
    # clear "couldn't fetch" message.
    curl -fsS -o "${MISC_SRC}/CImg/CImg.h" \
        "https://raw.githubusercontent.com/dtschump/CImg/${CIMG_VERSION}/CImg.h"
    curl -fsS -o "${MISC_SRC}/CImg/Inpaint/inpaint.h" \
        "https://raw.githubusercontent.com/dtschump/CImg/${CIMG_VERSION}/plugins/inpaint.h"
    patch -p0 -d "${MISC_SRC}/CImg" < "${MISC_SRC}/CImg/Inpaint/inpaint.h.patch" > /dev/null

    rm -rf "${MISC_BUILD}"
    cmake -S "${MISC_SRC}" -B "${MISC_BUILD}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH=/usr/local \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    cmake --build "${MISC_BUILD}" -j "$(nproc)"

    # --- openfx-arena ---------------------------------------------------------
    # The CMake build, not Makefile.master: the latter runs `pkg-config
    # OpenColorIO` unconditionally, and this image ships OCIO as a CMake
    # config package (libs/OpenColorIOConfig.cmake under /usr/local) with no
    # .pc file at all -- Makefile.master would fail before touching any of
    # openfx-arena's actual sources. The CMake build never needs OCIO in the
    # first place: GenericOCIO.cpp is only compiled under #ifdef
    # OFX_IO_USING_OCIO, which nothing here defines.
    #
    # PKG_CONFIG_PATH resolves Magick++/lcms2/libzip to the pinned builds
    # above; fontconfig, pangocairo, libxml-2.0, glib-2.0 and cairo all
    # resolve fine against the image's own packages, so nothing else needs
    # steering here.
    echo "[Plugins] building openfx-arena..."
    ARENA_SRC="${PLUGINS_SRC_DIR}/openfx-arena"
    ARENA_BUILD="${ARENA_SRC}/build"
    rm -rf "${ARENA_BUILD}"
    env PKG_CONFIG_PATH="${DEPS_PREFIX}/lib/pkgconfig:${PKG_CONFIG_PATH:-}" \
        cmake -S "${ARENA_SRC}" -B "${ARENA_BUILD}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH=/usr/local \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DWITH_SVG=OFF -DWITH_PDF=OFF -DWITH_CDR=OFF
    cmake --build "${ARENA_BUILD}" -j "$(nproc)"

    # All three projects' install targets already emit the OFX bundle layout
    # (<Name>.ofx.bundle/Contents/{Linux-x86-64,Resources,Info.plist}), which
    # is exactly what OFX_PLUGIN_PATH expects -- no hand-assembly needed.
    # openfx-misc's CMakeLists.txt installs two bundles (Misc.ofx.bundle and
    # CImg.ofx.bundle) into the same prefix as IO.ofx.bundle; the rm -rf
    # happens once, before any --install, so a run interrupted partway never
    # leaves PLUGINS_TARGET in a mixed old/new state.
    rm -rf "${PLUGINS_TARGET}"
    mkdir -p "${PLUGINS_TARGET}"
    cmake --install "${IO_BUILD}" --prefix "${PLUGINS_TARGET}" > /dev/null
    cmake --install "${MISC_BUILD}" --prefix "${PLUGINS_TARGET}" > /dev/null
    cmake --install "${ARENA_BUILD}" --prefix "${PLUGINS_TARGET}" > /dev/null

    for ofx in "${IO_OFX}" "${MISC_OFX}" "${CIMG_OFX}" "${ARENA_OFX}"; do
        if [ ! -f "${ofx}" ]; then
            echo "[Plugins] ERROR: build produced no ${ofx}" >&2
            exit 1
        fi
    done

    # Stage Arena.ofx's private libraries. `readelf -d` shows
    # RUNPATH=$ORIGIN/../../Libraries, and $ORIGIN is
    # Arena.ofx.bundle/Contents/Linux-x86-64 -- two levels up from there is
    # the bundle root, so the target is Arena.ofx.bundle/Libraries, *not*
    # Contents/Libraries (verified with LD_DEBUG=libs; the search path glibc
    # actually tries is .../Arena.ofx.bundle/Contents/Linux-x86-64/../../Libraries,
    # which collapses to .../Arena.ofx.bundle/Libraries). Only Magick++/
    # MagickWand/MagickCore, lcms2 and libzip need staging -- the same reason
    # IO.ofx's SeExpr is linked statically: this makes the bundle
    # self-contained with no LD_LIBRARY_PATH needed anywhere. Everything else
    # Arena.ofx links against (fontconfig, freetype, pango, cairo, glib,
    # libxml2, OpenGL) is already resolvable through the image's own library
    # set.
    ARENA_LIBRARIES="${PLUGINS_TARGET}/Arena.ofx.bundle/Libraries"
    mkdir -p "${ARENA_LIBRARIES}"
    cp -P "${DEPS_PREFIX}"/lib/libMagickCore-7.Q32HDRI.so.* "${ARENA_LIBRARIES}/"
    cp -P "${DEPS_PREFIX}"/lib/libMagickWand-7.Q32HDRI.so.* "${ARENA_LIBRARIES}/"
    cp -P "${DEPS_PREFIX}"/lib/libMagick++-7.Q32HDRI.so.* "${ARENA_LIBRARIES}/"
    cp -P "${DEPS_PREFIX}"/lib/liblcms2.so.2* "${ARENA_LIBRARIES}/"
    cp -P "${DEPS_PREFIX}"/lib/libzip.so.5* "${ARENA_LIBRARIES}/"

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

    # Same check for openfx-arena, for the two Magick-backed IDs. (A third ID,
    # fr.inria.openfx.ReadMisc, is asserted further down instead -- see the
    # comment by ARENA_PROBE_OUT for why `strings` can't see it.)
    ARENA_SYMBOLS="${ARENA_BUILD}/arena-symbols.txt"
    strings "${ARENA_OFX}" > "${ARENA_SYMBOLS}"
    for id in net.fxarena.openfx.Text net.fxarena.openfx.ReadPSD; do
        if ! grep -Fxq -- "${id}" "${ARENA_SYMBOLS}"; then
            echo "[Plugins] ERROR: built bundle does not export ${id}" >&2
            exit 1
        fi
    done

    echo "${PLUGINS_WANT}" > "${PLUGINS_STAMP}"
    echo "[Plugins] done -> ${PLUGINS_TARGET}"
fi

# ---------------------------------------------------------------------------
# verify_plugin_loads: dlopen() each built bundle for real and confirm it
# exports a working OfxGetNumberOfPlugins/OfxGetPlugin pair. This catches
# what the strings/symbol check above cannot: a bundle whose identifiers are
# present in the binary but that fails to dlopen (missing SONAME, unresolved
# symbol, etc).
# ---------------------------------------------------------------------------
VERIFY_LOADER_SRC="${REPO_ROOT}/tools/ci/verify_plugin_loads.cpp"
VERIFY_LOADER_BIN="${PLUGINS_SRC_DIR}/verify_plugin_loads"
HOSTSUPPORT_SRC_DIR="${REPO_ROOT}/libs/OpenFX/HostSupport/src"
HOSTSUPPORT_INC_DIR="${REPO_ROOT}/libs/OpenFX/HostSupport/include"
OFX_INC_DIR="${REPO_ROOT}/libs/OpenFX/include"
OFX_EXT_DIR="${REPO_ROOT}/libs/OpenFX_extensions"

mkdir -p "${PLUGINS_SRC_DIR}"

# The -D list mirrors HostSupport/CMakeLists.txt's add_library(HostSupport
# ...) PUBLIC compile definitions -- keep the two in sync if that target's
# definitions change.
if [ ! -x "${VERIFY_LOADER_BIN}" ] || [ "${VERIFY_LOADER_SRC}" -nt "${VERIFY_LOADER_BIN}" ]; then
    echo "[verify_plugin_loads] building probe..."
    g++ -std=c++20 -O2 \
        -I "${HOSTSUPPORT_INC_DIR}" -I "${OFX_INC_DIR}" -I "${OFX_EXT_DIR}" \
        -DOFX_EXTENSIONS_NUKE -DOFX_EXTENSIONS_TUTTLE -DOFX_EXTENSIONS_VEGAS \
        -DOFX_SUPPORTS_PARAMETRIC -DOFX_EXTENSIONS_NATRON -DOFX_EXTENSIONS_RESOLVE \
        -DOFX_SUPPORTS_OPENGLRENDER -DOFX_SUPPORTS_MULTITHREAD -DOFX_SUPPORTS_DIALOG \
        "${VERIFY_LOADER_SRC}" \
        "${HOSTSUPPORT_SRC_DIR}"/ofxh*.cpp \
        "${OFX_EXT_DIR}/ofxhParametricParam.cpp" \
        -lexpat -ldl \
        -o "${VERIFY_LOADER_BIN}"
fi

# Misc.ofx, CImg.ofx and Arena.ofx are all self-contained (see the
# "openfx-misc is built the same way" note above, and the library-staging
# comment in the openfx-arena build step), so a failed dlopen() here is
# always a real regression -- fail loudly.
for ofx in "${MISC_OFX}" "${CIMG_OFX}" "${ARENA_OFX}"; do
    echo "[verify_plugin_loads] probing ${ofx##*/}..."
    "${VERIFY_LOADER_BIN}" "${ofx}"
done

# fr.inria.openfx.ReadMisc (unconditional -- never gated by WITH_SVG/
# WITH_PDF/WITH_CDR, so its presence also proves the three disabled readers
# didn't silently take other targets down with them) can't be asserted via
# `strings` like the two IDs above: at this -Ofast optimization level, GCC
# builds this specific 24-byte std::string constant from a 16-byte prefix
# copy out of .rodata (shared with the other four "fr.inria.openfx."-prefixed
# IDs) plus the 8-byte tail "ReadMisc" folded into a `movabs` immediate
# operand in the code itself (confirmed by disassembling the bytes at that
# offset). The full identifier therefore never exists as one contiguous run
# anywhere in the file for `strings`/grep to find, even though it's exactly
# what a real host sees at runtime -- which is what this probe call already
# gives us for free. Same anti-pipefail discipline as the `strings` checks:
# capture to a file first, `grep -Fxq` the file, never `| grep -q` directly.
ARENA_PROBE_OUT="${PLUGINS_SRC_DIR}/arena-probe-out.txt"
"${VERIFY_LOADER_BIN}" "${ARENA_OFX}" > "${ARENA_PROBE_OUT}"
if ! grep -Fq -- "fr.inria.openfx.ReadMisc " "${ARENA_PROBE_OUT}"; then
    echo "[verify_plugin_loads] ERROR: ${ARENA_OFX##*/} does not report plugin fr.inria.openfx.ReadMisc" >&2
    exit 1
fi

# IO.ofx is not: `readelf -d` shows RUNPATH=$ORIGIN/../../Libraries, but
# neither this script nor openfx-io's own install target populates
# Contents/Libraries/ with libOpenColorIO/libOpenImageIO/libOpenEXR/etc, so
# a bare dlopen() only succeeds if the container's dynamic linker already
# resolves those SONAMEs some other way (e.g. ldconfig having indexed
# /usr/local/lib). That is true or false depending on the container, not on
# this script, so treat the result as informational rather than gating on
# it here.
echo "[verify_plugin_loads] probing ${IO_OFX##*/} (informational -- see comment above)..."
if ! "${VERIFY_LOADER_BIN}" "${IO_OFX}"; then
    echo "[verify_plugin_loads] WARNING: ${IO_OFX##*/} did not dlopen cleanly; not failing the build for it (see comment above)." >&2
fi

echo "== fetch-assets.sh: all assets present =="
