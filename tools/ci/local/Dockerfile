# Local dev image mirroring the CI build environment as closely as possible.
#
# Base image and package list here MUST match .github/workflows/ci.yml so
# that a build inside this container reproduces CI exactly. Do not bump the
# base image tag independently of ci.yml, and keep the CI package list
# documentation below in sync with the "Install Linux system packages" step
# there.
#
# Everything ci.yml's package-install step needs (bar one documented
# exception below) is already present in aswf/ci-vfxall:2027-clang21.1, so
# the only layers this file adds are the local-only Mesa relink and the
# Slang and glslang toolchains at the bottom, which CI never sees (CI runs the
# base image directly, and downloads and caches them itself through
# cmake/FetchSlang.cmake and cmake/FetchGlslang.cmake). It is kept as a file
# (rather than building the image straight from `docker pull`) so that:
#   (a) the drift-check comment block below has somewhere to live, and
#   (b) local-only layers have an obvious, already-wired seam to land in.
#
# Why ci-vfxall and not ci-baseqt (which this used through M7): ci-baseqt
# ships Qt/PySide and the base libraries but no OpenColorIO, OpenImageIO, or
# OpenFX, which M3's OCIO work and M11's plugin integration test both need.
# ci-vfxall is the same ci-common:7-clang21 base with builds of every ASWF
# package deployed into /usr/local instead of just the Qt/PySide subset. The
# RPM layer is therefore byte-identical between the two images (both are
# FROM ci-common:7-clang21 and neither adds distro packages of its own), and
# every Conan package version is shared, because both read the same "2027"
# package_versions block in aswf-docker's versions.yaml.
FROM aswf/ci-vfxall:2027-clang21.1

# --- CI package list, for drift-checking against ci.yml -------------------
#
# IMPORTANT: this environment has no reachable distro package repository
# (mirrors.rockylinux.org, mirrors.fedoraproject.org, dl.fedoraproject.org,
# and every EPEL mirror probed are all unreachable; only github.com,
# registry-1.docker.io, auth.docker.io, mirror.gcr.io, ghcr.io, and
# quay.io are reachable). `dnf install` therefore CANNOT be run here in any
# form -- not with retries, not against alternate mirrors. This section is
# NOT executable; it exists purely so a future reader diffing this file
# against ci.yml can tell whether the two have drifted.
#
# ci.yml's "Install Linux system packages" step currently installs (union of
# the two branches that carry slightly different lists -- RB-2.6 includes
# wayland-devel, ci-smoke-test-m2p3t1a does not; the aswf base image
# already satisfies both variants, see below):
#
#   dnf install -y cairo-devel epel-release wayland-devel wget unzip \
#                   clang xorg-x11-server-Xvfb
#
# Status of each against this base image (aswf/ci-vfxall:2027-clang21.1).
# Every RPM below comes from the shared ci-common:7-clang21 layer, so these
# statuses carried over unchanged from ci-baseqt:2027.0:
#   cairo-devel              -- already installed as an RPM.
#   epel-release             -- not needed here.
#   wayland-devel            -- already installed as an RPM (only present on
#                                the RB-2.6-style package list; harmless
#                                either way since it's already there). Not
#                                consumed by the build: Natron does not
#                                detect or link against Wayland.
#   wget                     -- already installed as an RPM.
#   unzip                    -- already installed as an RPM.
#   clang                    -- present at /usr/local/bin via Conan, so
#                                `rpm -q clang` would report it missing even
#                                though the binary is on PATH.
#   xorg-x11-server-Xvfb     -- already installed as an RPM.
#
# If ci.yml's package list changes, re-check this table -- most items are
# almost certainly already covered by the base image, but confirm before
# assuming so.

# --- Local-only additions (not part of CI's package list) ------------------
# ccache, ninja, and gdb are already present in this base image (ccache and
# ninja via Conan at /usr/local/bin, gdb as an RPM) -- nothing to install.

# --- Mesa: bind its drivers to the system LLVM -----------------------------
# Mesa's RPMs were built against the system /usr/lib64/libLLVM.so.21.1, but
# both ld.so.conf and LD_LIBRARY_PATH put /usr/local/lib first, where the
# Conan clang toolchain ships its own libLLVM.so.21.1 under the same soname
# without the AMDGPU target. RADV and radeonsi then fail to load
# (`undefined symbol: LLVMInitializeAMDGPUTargetInfo`), leaving only the
# software rasterisers. Neither the search order nor LD_PRELOAD can fix
# that globally: clang itself links the /usr/local copy, and Qt needs
# /usr/local ahead of /usr/lib64 for FreeType.
#
# A NEEDED entry containing a slash is opened by that exact path, bypassing
# every search path, so rewriting it in each Mesa library that links LLVM
# pins only Mesa to the system copy and leaves the toolchain untouched. All
# such libraries are rewritten, llvmpipe and lavapipe included, so a process
# that loads several Mesa drivers maps one LLVM rather than two copies that
# export identically versioned symbols. The build fails if nothing matched,
# so a base image bump that renames these libraries cannot silently undo it.
RUN set -eu; \
    sys_llvm=/usr/lib64/libLLVM.so.21.1; \
    patched=0; \
    for lib in $(rpm -ql $(rpm -qa 'mesa-*') | grep '\.so' | sort -u); do \
        if [ -f "$lib" ] && [ ! -L "$lib" ] \
            && readelf -d "$lib" 2>/dev/null | grep -q 'NEEDED.*\[libLLVM\.so\.21\.1\]'; then \
            patchelf --replace-needed libLLVM.so.21.1 "$sys_llvm" "$lib"; \
            echo "relinked $lib -> $sys_llvm"; \
            patched=$((patched + 1)); \
        fi; \
    done; \
    test "$patched" -gt 0; \
    ldd /usr/lib64/libvulkan_radeon.so | grep -q "$sys_llvm"; \
    ! ldd /usr/lib64/libvulkan_radeon.so | grep -q /usr/local/lib/libLLVM

# --- Slang: bake the shader compiler so configure never downloads it -------
# Same release cmake/FetchSlang.cmake pins; tools/ci/local/check-image-pins.sh
# fails CI when the two drift. SLANG_ROOT makes FetchSlang.cmake use this copy
# instead of fetching into .natron-deps. Only slangc and the prelude header
# are consumed, so the build fails if a release layout change drops either.
ARG NATRON_SLANG_VERSION=2026.19
ARG NATRON_SLANG_SHA256=bd6cfc47b7353b2cffa36a866b1584749f76f4b2dbebf6dda26bc3fb40dc3c0e
ARG NATRON_SLANG_URL=https://github.com/shader-slang/slang/releases/download/v${NATRON_SLANG_VERSION}/slang-${NATRON_SLANG_VERSION}-linux-x86_64.tar.gz
RUN set -eu; \
    tarball=/tmp/slang.tar.gz; \
    wget -q -O "$tarball" "$NATRON_SLANG_URL"; \
    echo "${NATRON_SLANG_SHA256}  ${tarball}" | sha256sum -c -; \
    mkdir -p /opt/slang; \
    tar -xzf "$tarball" -C /opt/slang; \
    rm -f "$tarball"; \
    test -x /opt/slang/bin/slangc; \
    test -f /opt/slang/include/slang-cpp-prelude.h
ENV SLANG_ROOT=/opt/slang

# --- glslang: bake the source tree so configure never downloads it ---------
# Same release cmake/FetchGlslang.cmake pins; check-image-pins.sh fails CI when
# the two drift. FetchGlslang.cmake compiles glslang as part of the Natron
# build, so this is the source tree, not a prebuilt toolchain. GLSLANG_ROOT
# makes it use this copy instead of fetching into .natron-deps. The build fails
# if the archive layout drops the two files FetchGlslang.cmake validates.
ARG NATRON_GLSLANG_VERSION=16.6.0
ARG NATRON_GLSLANG_SHA256=9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2
ARG NATRON_GLSLANG_URL=https://github.com/KhronosGroup/glslang/archive/refs/tags/${NATRON_GLSLANG_VERSION}.tar.gz
RUN set -eu; \
    tarball=/tmp/glslang.tar.gz; \
    wget -q -O "$tarball" "$NATRON_GLSLANG_URL"; \
    echo "${NATRON_GLSLANG_SHA256}  ${tarball}" | sha256sum -c -; \
    mkdir -p /opt/glslang; \
    tar -xzf "$tarball" -C /opt/glslang --strip-components=1; \
    rm -f "$tarball"; \
    test -f /opt/glslang/CMakeLists.txt; \
    test -f /opt/glslang/glslang/Public/ShaderLang.h
ENV GLSLANG_ROOT=/opt/glslang
