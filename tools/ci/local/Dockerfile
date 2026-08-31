# Local dev image mirroring the CI build environment as closely as possible.
#
# Base image and package list here MUST match .github/workflows/ci.yml so
# that a build inside this container reproduces CI exactly. Do not bump the
# base image tag independently of ci.yml, and keep the CI package list
# documentation below in sync with the "Install Linux system packages" step
# there.
#
# This file currently adds no layers on top of the base image -- everything
# ci.yml's package-install step needs (bar one documented exception below) is
# already present in aswf/ci-baseqt:2027.0. It is kept as a file (rather than
# building the image straight from `docker pull`) so that:
#   (a) the drift-check comment block below has somewhere to live, and
#   (b) future local-only layers (additional tools, env setup, etc.) have an
#       obvious, already-wired seam to land in instead of a fresh file.
FROM aswf/ci-baseqt:2027.0

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
# wayland-devel, ci-smoke-test-m2p3t1a does not; the aswf/ci-baseqt base
# image already satisfies both variants, see below):
#
#   dnf install -y cairo-devel epel-release wayland-devel wget unzip \
#                   clang xorg-x11-server-Xvfb
#   dnf install -y extra-cmake-modules
#
# Status of each against this base image (aswf/ci-baseqt:2027.0):
#   cairo-devel              -- already installed as an RPM.
#   epel-release             -- not needed here: its only purpose in ci.yml
#                                is to make extra-cmake-modules resolvable
#                                from EPEL, and we are not installing
#                                extra-cmake-modules at all (see "Known
#                                divergence from CI" below).
#   wayland-devel            -- already installed as an RPM (only present on
#                                the RB-2.6-style package list; harmless
#                                either way since it's already there).
#   wget                     -- already installed as an RPM.
#   unzip                    -- already installed as an RPM.
#   clang                    -- present at /usr/local/bin via Conan, so
#                                `rpm -q clang` would report it missing even
#                                though the binary is on PATH.
#   xorg-x11-server-Xvfb     -- already installed as an RPM.
#   extra-cmake-modules      -- NOT PROVIDED by this image. See "Known
#                                divergence from CI" below.
#
# If ci.yml's package list changes, re-check this table -- most items are
# almost certainly already covered by the base image, but confirm before
# assuming so.

# --- Known divergence from CI: extra-cmake-modules is NOT installed --------
#
# ci.yml installs extra-cmake-modules (ECM) via `dnf install`. This image
# does not provide it, and we are deliberately not building it from source
# here: this environment has no reachable distro package repository (see
# above), and the project decided to use the aswf/ci-baseqt image as-is
# rather than carry a from-source build of one package. A from-source build
# of ECM v5.116.0 was prototyped and confirmed to work (see below) before
# this decision was made to cut it.
#
# Concrete consequence for local builds: CMakeLists.txt (around line 114)
# does, under `if(UNIX AND NOT APPLE)`:
#   find_package(ECM NO_MODULE)
#   set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${CMAKE_MODULE_PATH})
#   find_package(X11 REQUIRED)
#   find_package(Wayland COMPONENTS Client Egl)
# ECM ships find-modules/FindWayland.cmake, which is what lets the
# find_package(Wayland ...) call above succeed. Without ECM installed,
# ECM_MODULE_PATH is empty and CMake has no FindWayland module on its path.
# Neither find_package(ECM ...) nor find_package(Wayland ...) is REQUIRED,
# so this does NOT break the configure step or the build -- it just silently
# resolves Wayland support to off in a local build, while CI (which does
# install ECM) configures with Wayland support on. wayland-devel itself IS
# installed in this image, so this is purely a missing-build-tool gap, not a
# real absence of Wayland on the system. Keep this in mind when comparing
# local build results/behavior against CI -- it is a silent difference, not
# a build failure, so it's easy to miss.
#
# How to close this gap if it ever matters: install extra-cmake-modules from
# EPEL (not reachable from this environment, but may be elsewhere), or build
# it from github.com/KDE/extra-cmake-modules directly. v5.116.0 (the final
# KDE Frameworks 5 (KF5) release of ECM, Nov 2023 -- matching what EPEL 9's
# KF5-era extra-cmake-modules package would pull into ci.yml's GitHub
# runner) was verified to build cleanly with this image's own cmake/ninja,
# to ship find-modules/FindWayland.cmake, and to result in a configure where
# Wayland_FOUND=TRUE.

# --- Local-only additions (not part of CI's package list) ------------------
# ccache, ninja, and gdb are already present in this base image (ccache and
# ninja via Conan at /usr/local/bin, gdb as an RPM) -- nothing to install
# here. This section is intentionally empty; kept as a marker in case a
# future local-only tool needs adding, so it's obvious where it belongs.
#
# `qtpy` (M2.P3.T1d) is deliberately NOT installed here either, even though
# it doesn't need dnf/a distro repo (it's `pip install`-able and PyPI IS
# reachable from this environment, unlike distro mirrors). This file is
# FROM aswf/ci-baseqt:2027.0 and adds no layers, but CI never builds or
# pulls this image -- ci.yml runs `aswf/ci-baseqt:2027.0` directly as its
# job container (see README.md). A layer added here would fix the local
# loop and leave `import qtpy` broken in CI. `qtpy` is instead installed by
# tools/ci/local/fetch-assets.sh, which both CI and the local loop actually
# run -- see that script's "Python test dependencies" section.
