---
title: Linux-Only Qt6 Foundation Plan
status: running
current: M0.P1.T5
pm_heartbeat: 2026-08-29T22:20:00+00:00
ship: pr-per-milestone
---

# Goal

Fork `NatronGitHub/Natron`, drop Windows/macOS and Qt5, move to C++20 with
current dependencies, and stand up CI/CD that actually gates merges — so
future core work has solid ground to build on.

# Context and constraints

| | |
|---|---|
| **From** | Qt5.15 / C++17 |
| **To** | Qt6.8.x / C++20 |
| **OS baseline** | Rocky Linux 9 (EL9) |
| **Toolchain** | gcc 14.2 / glibc 2.34 |
| **Build system** | CMake only |
| **Milestones** | 7 (M0–M6) |

- **Why this is tractable:** the two hardest Qt6 blockers — the
  `QOpenGLWidget` viewer and a working `NATRON_QT6` CMake switch — are already
  merged upstream. Because this fork **drops Qt5 entirely** rather than
  supporting both, it's simpler than upstream's own plan: no
  `#if QT_VERSION` guards to write, no dual-toolkit CI matrix to maintain, no
  macOS/Windows packaging to keep in sync. This is closer to deleting a
  branch than porting one.
- The dependency baseline targets the VFX Reference Platform's **CY2027
  draft** direction rather than CY2026-final — see
  `PLAN/DECISIONS/2026-08-29-target-vfx-cy2027.md`.
- **Sequencing:** `M0 (fork & cut) → M1 (toolchain) → M2 (Qt6) / M3 (deps,
  parallel) → M4 (CI/CD) → M5 (tests & P0s, ongoing)`. M2 and M3 run in
  parallel on separate branches. M4 starts as soon as M1 lands, so every
  M2/M3 PR gets gated automatically. M6 (docs) comes last, once the build is
  actually the thing being documented.
- Grounded in the `RB-2.6` tree (`CMakeLists.txt`, `INSTALL_LINUX.md`,
  `Global/Macros.h`, `tools/jenkins/`), the open PR queue on
  `NatronGitHub/Natron`, and the ASWF `aswf-docker` image catalog, as of
  2026-08-29. Re-check specific line numbers, PR states, and image tags
  before executing — all three will have moved.

# Board

| ID | Milestone | Status | File |
|----|-----------|--------|------|
| M0 | Fork & cut scope | doing | [M0-fork-cut-scope.md](PLAN/MILESTONES/M0-fork-cut-scope.md) |
| M1 | Toolchain baseline | todo | [M1-toolchain-baseline.md](PLAN/MILESTONES/M1-toolchain-baseline.md) |
| M2 | Land the Qt6 migration | todo | [M2-qt6-migration.md](PLAN/MILESTONES/M2-qt6-migration.md) |
| M3 | Dependency modernization | todo | [M3-dependency-modernization.md](PLAN/MILESTONES/M3-dependency-modernization.md) |
| M4 | CI/CD rebuild | todo | [M4-cicd-rebuild.md](PLAN/MILESTONES/M4-cicd-rebuild.md) |
| M5 | Test & correctness baseline | todo | [M5-test-correctness-baseline.md](PLAN/MILESTONES/M5-test-correctness-baseline.md) |
| M6 | Documentation pass | todo | [M6-documentation-pass.md](PLAN/MILESTONES/M6-documentation-pass.md) |

# Open questions

(none)
