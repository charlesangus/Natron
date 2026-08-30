# Milestone 1: Toolchain baseline

`~2-3 days` · low risk. Set the floor before porting code to it.
`CMakeLists.txt` already isolates the Qt5/Qt6 choice behind one
Qt-version-switch `if()` block — this milestone collapses it to one path.

## Phase 1.1: Collapse to one toolchain

- [x] M1.P1.T1 — Bump `CMAKE_CXX_STANDARD` 17 → 20
  - files: `CMakeLists.txt:29`
  - approach: Devernay's own macOS packaging build already compiles at
    `-std=c++20` (poppler requires it) — this just makes it the project-wide
    minimum, not a packaging-only override.
  - verify: `cmake --build` reconfigures with `CMAKE_CXX_STANDARD=20` and the
    project still compiles.
  - size: S

- [x] M1.P1.T2 — Delete the Qt5/PySide2/Shiboken2 branch
  - files: `CMakeLists.txt:86-102`
  - approach: removed the `else()` branch and the Qt6-toggle option itself.
    Hard-require **Qt 6.8.x** specifically (not just "6.3+") to match the VFX
    Reference Platform pin, plus Shiboken6/PySide6 6.8.x.
  - verify: CMake configure fails clearly if Qt5 is on `CMAKE_PREFIX_PATH`
    instead of Qt6; succeeds against Qt 6.8.x.
  - size: M

- [x] M1.P1.T3 — Pin the baseline: `aswf/ci-baseqt:2027`
  - files: none (image selection, consumed by M3/M4)
  - approach: per decision
    `PLAN/DECISIONS/2026-08-29-target-vfx-cy2027.md` — glibc 2.34, gcc 14.2
    (Rocky 9). No custom image needed: the Academy Software Foundation
    publishes ready-made, versioned CI images for exactly this
    (`aswf-docker`) — `ci-common:7` is the bare Rocky-9/gcc-14.2 toolchain,
    `ci-base:2027` layers on the pinned VFX Platform library stack, and
    `ci-baseqt:2027` adds Qt 6.8.x + PySide6/Shiboken6 on top. Use
    `ci-baseqt:2027` directly rather than building one — carried into M3 and
    M4.
  - verify: n/a — recorded decision; consumed as `container:` value in M4's
    workflow.
  - size: S

- [ ] M1.P1.T4 — Audit `Global/Macros.h` version gates
  - files: `Global/Macros.h`
  - approach: delete the explicit `#if __cplusplus <= 201103L` /
    `<= 201402L` pre-C++17 fallback code — dead weight once C++20 is the
    floor; safe, mechanical deletion.
  - verify: `git grep` for `__cplusplus <= 201103L` / `<= 201402L` in
    `Global/Macros.h` returns nothing; project still compiles.
  - size: S

**Verification gate:** CMake configures and builds the project against Qt
6.8.x and C++20 only, with no Qt5/PySide2/Shiboken2 code path or pre-C++17
compatibility shims remaining.
