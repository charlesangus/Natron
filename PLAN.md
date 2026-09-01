---
title: Linux-Only Qt6 Foundation Plan
status: running
current: M3.P1.T9
pm_heartbeat: 2026-09-01T11:40:00-04:00
ship: pr-per-milestone
publish_decisions: docs/decisions/
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
- **Sequencing:** `M0 (fork & cut) → M1 (toolchain) → M2 (Qt6) → M7/M8 (local
  builds, branching) → M10 (clean-sheet CI/CD) → M3 (deps) → M5 (tests & P0s,
  ongoing)`.
  **M10 shipped 2026-08-31** (PR #7, merge `c43270bc1`): `main` now requires
  `format`, `lint-ci` and `build-and-test`, and the `ci` aggregator is gone. M6 (docs) comes last, once the build is actually the thing being
  documented. **M9 is cancelled** and no longer gates M10; the "Fetch test
  assets" step it would have deleted is now load-bearing — it builds the OFX
  plugin bundle from pinned source (see
  `DECISIONS/2026-08-31-restore-vendored-ofx-plugin-tests.md`), so M10 should
  design around it, not around its removal. M11 is largely delivered by the
  same change and needs rescoping to rendering + video I/O. Board rows below
  are listed in execution order, not ID order.
- **M2 is done** (2026-08-31). It was parked twice — for M7 (the local build
  loop, which removed the CI round-trip that made `M2.P3.T1a` uneconomic) and
  then for M8 (which restored a merge path after a job rename broke branch
  protection). Both have shipped. Its PR then sat red on a failure that was
  inherited rather than Qt6 scope — 3 failures, all `BaseTest`, all on the
  vendored OFX plugin bundle failing to load. **Resolved 2026-08-31** — the
  cause was the container, not the tests: on `aswf/ci-vfxall` with the bundle
  built from source, all 28 ctest cases pass. **Shipped 2026-08-31** — PR #6
  went green end to end and squash-merged as `88e3ab05a`. See
  `DECISIONS/2026-08-31-restore-vendored-ofx-plugin-tests.md` and
  `DECISIONS/2026-08-31-switch-ci-image-to-vfxall.md`.
- **The plan lives on the orphan `plan` branch**, checked out at `.plan/`
  (PLAN-FORMAT.md §1a). Commit code first, then the plan, per §9 — plan edits
  never ride in a code commit or a PR diff. See
  `DECISIONS/2026-08-31-migrate-plan-worktree.md`.
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
| M2 | Land the Qt6 migration | done    | [M2-qt6-migration.md](PLAN/MILESTONES/M2-qt6-migration.md) |
| M7 | Local incremental builds | done | [M7-local-incremental-builds.md](PLAN/MILESTONES/M7-local-incremental-builds.md) |
| M8 | Branching model and CI/CD rebuild | done | [M8-branching-and-cicd.md](PLAN/MILESTONES/M8-branching-and-cicd.md) |
| M9 | Drop the vendored OFX plugin dependency | cancelled | [M9-drop-vendored-ofx.md](PLAN/MILESTONES/M9-drop-vendored-ofx.md) |
| M10 | Clean-sheet CI/CD | done | [M10-cicd-clean-sheet.md](PLAN/MILESTONES/M10-cicd-clean-sheet.md) |
| M3 | Dependency modernization | doing | [M3-dependency-modernization.md](PLAN/MILESTONES/M3-dependency-modernization.md) |
| M4 | CI/CD rebuild | done | [M4-cicd-rebuild.md](PLAN/MILESTONES/M4-cicd-rebuild.md) |
| M5 | Test & correctness baseline | todo | [M5-test-correctness-baseline.md](PLAN/MILESTONES/M5-test-correctness-baseline.md) |
| M6 | Documentation pass | todo | [M6-documentation-pass.md](PLAN/MILESTONES/M6-documentation-pass.md) |
| M11 | OFX plugin integration test (pre-release) | rescope | [M11-ofx-plugin-integration-test.md](PLAN/MILESTONES/M11-ofx-plugin-integration-test.md) |

# Open questions

_None awaiting a human answer._ The five that paused this run on 2026-08-31
were answered the same day; see `DECISIONS/2026-08-31-aces-via-ocio-builtin-config.md`,
`-build-openfx-misc-in-ci.md`, `-drop-wayland-support.md` and
`-delete-dead-platform-files.md`.
