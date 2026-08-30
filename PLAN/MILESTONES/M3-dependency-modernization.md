# Milestone 3: Dependency modernization

`~3-5 days` · low-medium risk · runs in parallel with M2 on a separate branch.

Much lighter than initially scoped: `aswf/ci-baseqt:2027` already *is* the
pinned VFX-Platform dependency set — Boost 1.88, OpenColorIO 2.5.x, OpenEXR
3.4.x, Imath 3.2.x, OpenVDB 13.x, Alembic 1.8.x, Qt 6.8.x, PySide6/Shiboken6
6.8.x, all pre-built into `/usr/local`. No custom base image, no building
libraries from source, no Dockerfile to maintain. This milestone is now mostly
about pointing Natron's build at that image and handling what's specific to
Natron.

**Risk note:** this milestone is still betting on the CY2027 draft's
direction (`PLAN/DECISIONS/2026-08-29-target-vfx-cy2027.md`), but the
fallback is cheap — if Rocky 9/glibc 2.34 turns out premature, swapping to
`aswf/ci-baseqt:2026` (Rocky 8/glibc 2.28, CY2026-final) is a one-line tag
change in M4's CI, not a rebuild of custom infrastructure.

## Phase 3.1: Point the build at the ASWF image

- [ ] M3.P1.T1 — Wire CMake's `find_package` calls at the ASWF-image paths
  - files: `CMakeLists.txt` and any `Find*.cmake` modules under the repo
  - approach: everything lands in `/usr/local` in the image rather than
    distro-standard paths — set `CMAKE_PREFIX_PATH`/`PKG_CONFIG_PATH`
    accordingly. This replaces the entire `NATRON_SYSTEM_LIBS` vs.
    custom-SDK question — it's neither; it's "build against what ASWF ships."
  - verify: CMake configure inside `aswf/ci-baseqt:2027` finds all
    dependencies with no manual path overrides beyond
    `CMAKE_PREFIX_PATH`/`PKG_CONFIG_PATH`.
  - size: S

- [ ] M3.P1.T2 — Confirm exact pinned versions with `ci-baseqt:2027`'s package manifest
  - files: none (verification against the image's own docs/labels)
  - approach: the "2027" tag tracks the draft — re-check versions against the
    image's own docs/labels before relying on specific numbers in code or
    docs, since the draft (and the image) can still move before Jan 2027.
  - verify: version numbers referenced in M3/M6 docs match what
    `docker run aswf/ci-baseqt:2027` actually reports.
  - size: S

- [ ] M3.P1.T3 — Swap the OCIO config default to ACES 2.0
  - files: OCIO config wiring (wherever the current default config path/URL
    is set — the `OpenColorIO-Configs` tarball reference)
  - approach: Natron currently ships a 2018-era `OpenColorIO-Configs` tarball
    (blender/natron/nuke-default profiles) — a separate download, not
    something the ASWF image provides. ACES 2.0 is the VFX-Platform-mandated
    default for CY2026+; this is a color-science-visible change to the
    out-of-the-box experience, not just a build detail. Flag it in release
    notes.
  - verify: a fresh install opens with ACES 2.0 as the active OCIO config;
    release notes mention the change.
  - size: M

- [ ] M3.P1.T4 — Check plugin-repo compatibility
  - files: none in this repo (verification against `openfx-io`/`openfx-misc`)
  - approach: `openfx-io` and `openfx-misc` are separate repos (not
    submodules) that CI currently downloads as prebuilt zips. No Qt
    dependency, but confirm they build cleanly against the image's gcc
    14.2/C++20/OpenColorIO 2.5/OpenEXR 3.4 before wiring them into the new CI
    (M4).
  - verify: both plugin repos build successfully inside
    `aswf/ci-baseqt:2027`.
  - size: S

**Verification gate:** the project builds cleanly inside
`aswf/ci-baseqt:2027` with no distro-package fallbacks, ACES 2.0 is the
default OCIO config, and `openfx-io`/`openfx-misc` build against the same
image.
