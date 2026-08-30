# Pin the exact ASWF image tag: `aswf/ci-baseqt:2027.1`, not `2027`

2026-08-29. M1 named the baseline image as `aswf/ci-baseqt:2027` (shorthand for
"whatever CY2027 image exists"). Checked the actual `aswf-docker` repo
(`AcademySoftwareFoundation/aswf-docker`, `ci-baseqt/README.md`) while
implementing M4: published tags carry a minor suffix per release within the
year — the current one is **`aswf/ci-baseqt:2027.1`** — there is no bare
`2027` tag.

Confirmed image contents for `2027.1`: Qt 6.8.3, PySide 6.8.3, Boost 1.91.0,
CPython 3.13.14, Expat 2.8.2, built on Rocky Linux 9 (`ci-common:7`), with
**GCC 14.2 as the default compiler on `PATH`** (via `gcc-toolset-14`, ahead of
the also-installed Clang) — matches the `gcc 14.2 / glibc 2.34` toolchain
target in `PLAN/DECISIONS/2026-08-29-target-vfx-cy2027.md` without any extra
activation step. Qt 6.8.3 satisfies CMake's `find_package(Qt6 6.8 ...)` pin
from M1.

Not covered by the image (Natron-specific, not part of the VFX Reference
Platform): `cairo` (roto/paint rasterization) and `extra-cmake-modules` (needs
EPEL on Rocky 9). M4's CI workflow installs these via `dnf` alongside the
image's baked-in stack.

Decision: use `aswf/ci-baseqt:2027.1` verbatim in CI, not `2027`. Re-verify
the exact tag again before any future bump (VFX Platform year rolls, or the
draft direction changes per the CY2027 decision's fallback).
