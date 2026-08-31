---
title: Linux-Only Qt6 Foundation Plan
status: running
current: M2.P3.T1
pm_heartbeat: 2026-08-31T05:51:51-04:00
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
| **Milestones** | 9 (M0–M8) |

- **Why this is tractable:** the two hardest Qt6 blockers — the
  `QOpenGLWidget` viewer and a working Qt6 CMake path — are already
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
- **M2 is resumed and running** (2026-08-31), at `M2.P3.T1c`. It was parked
  twice — for M7 (the local build loop, which removed the CI round-trip that
  made `M2.P3.T1a` uneconomic) and then for M8 (which restored a merge path
  after a job rename broke branch protection). Both have shipped. M2's work
  was rebased off `main` onto `milestone/m2-qt6-migration`; see that
  milestone's `## Decisions`. Board rows below are listed in execution order,
  not ID order.
- Grounded in the `RB-2.6` tree (`CMakeLists.txt`, `INSTALL_LINUX.md`,
  `Global/Macros.h`, `tools/jenkins/`), the open PR queue on
  `NatronGitHub/Natron`, and the ASWF `aswf-docker` image catalog, as of
  2026-08-29. Re-check specific line numbers, PR states, and image tags
  before executing — all three will have moved.

# Board

| ID | Milestone | Status | File |
|----|-----------|--------|------|
| M0 | Fork & cut scope | done | [M0-fork-cut-scope.md](PLAN/MILESTONES/M0-fork-cut-scope.md) |
| M1 | Toolchain baseline | done | [M1-toolchain-baseline.md](PLAN/MILESTONES/M1-toolchain-baseline.md) |
| M2 | Land the Qt6 migration | doing   | [M2-qt6-migration.md](PLAN/MILESTONES/M2-qt6-migration.md) |
| M7 | Local incremental builds | done | [M7-local-incremental-builds.md](PLAN/MILESTONES/M7-local-incremental-builds.md) |
| M8 | Branching model and CI/CD rebuild | done | [M8-branching-and-cicd.md](PLAN/MILESTONES/M8-branching-and-cicd.md) |
| M3 | Dependency modernization | todo | [M3-dependency-modernization.md](PLAN/MILESTONES/M3-dependency-modernization.md) |
| M4 | CI/CD rebuild | done | [M4-cicd-rebuild.md](PLAN/MILESTONES/M4-cicd-rebuild.md) |
| M5 | Test & correctness baseline | todo | [M5-test-correctness-baseline.md](PLAN/MILESTONES/M5-test-correctness-baseline.md) |
| M6 | Documentation pass | todo | [M6-documentation-pass.md](PLAN/MILESTONES/M6-documentation-pass.md) |

# Open questions

- **Which `aswf/ci-baseqt` tag is canonical — `2027.0` or `2027.1`?**
  `.github/workflows/ci.yml` pins `2027.0`, but
  `PLAN/DECISIONS/2026-08-29-pin-exact-aswf-tag.md` records `2027.1` as the
  chosen tag. M7 deliberately matches `ci.yml` at `2027.0` so the local
  environment reproduces CI exactly; bumping both is a separate change that
  should not land while M2's smoke-test failure is still being diagnosed.
  Decide after M2 closes.
