# Pin the exact ASWF image tag: `aswf/ci-baseqt:2027.0`, not `2027`

2026-08-29. M1 named the baseline image as `aswf/ci-baseqt:2027` (shorthand for
"whatever CY2027 image exists"). Checked the actual `aswf-docker` repo
(`AcademySoftwareFoundation/aswf-docker`) while implementing M4: published
tags carry a minor suffix per release within the year.

**Correction (same day, after a real CI run):** the first pass of this
decision named `2027.1`, read from `CHANGELOG.md`'s "2027.1 releases"
section. That section documents recipe/version changes *merged into the
build config*, not necessarily *published as a pushed image* — the actual
CI run failed with `manifest for aswf/ci-baseqt:2027.1 not found`. Checking
`gh api repos/AcademySoftwareFoundation/aswf-docker/releases` (which fires
from their automated publish workflow, one release object per pushed image)
shows the latest `aswf/ci-baseqt` release actually published is
**`aswf/ci-baseqt/2027.0`**, on 2026-06-30 — `2027.1` hadn't been built and
pushed yet as of 2026-08-29. Lesson: trust the releases list (what was
actually published), not the changelog (what was merged toward the next
release).

Confirmed image contents for the `2027.x` series (from `ci-baseqt/README.md`,
close enough between `.0` and the `.1` changelog deltas that none of it
changes the CI adaptation): Qt 6.8.3, PySide 6.8.3, Boost 1.91.0, CPython
3.13.14, Expat ~2.8.x, built on Rocky Linux 9 (`ci-common:7`), with **GCC
14.2 as the default compiler on `PATH`** (via `gcc-toolset-14`, ahead of the
also-installed Clang) — matches the `gcc 14.2 / glibc 2.34` toolchain target
in `PLAN/DECISIONS/2026-08-29-target-vfx-cy2027.md` without any extra
activation step. Qt 6.8.3 satisfies CMake's `find_package(Qt6 6.8 ...)` pin
from M1.

Not covered by the image (Natron-specific, not part of the VFX Reference
Platform): `cairo` (roto/paint rasterization) and `extra-cmake-modules` (needs
EPEL on Rocky 9). M4's CI workflow installs these via `dnf` alongside the
image's baked-in stack.

Decision: use `aswf/ci-baseqt:2027.0` verbatim in CI, not `2027` or `2027.1`.
Before any future bump (VFX Platform year rolls, `2027.1` actually publishes,
or the draft direction changes per the CY2027 decision's fallback),
re-verify against the **releases list**, not the changelog.
