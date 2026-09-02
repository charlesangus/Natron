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
# already present in aswf/ci-vfxall:2027-clang21.1. It is kept as a file
# (rather than building the image straight from `docker pull`) so that:
#   (a) the drift-check comment block below has somewhere to live, and
#   (b) future local-only layers (additional tools, env setup, etc.) have an
#       obvious, already-wired seam to land in instead of a fresh file.
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
# ninja via Conan at /usr/local/bin, gdb as an RPM) -- nothing to install
# here. This section is intentionally empty; kept as a marker in case a
# future local-only tool needs adding, so it's obvious where it belongs.
