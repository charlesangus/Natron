---
title: Linux-Only Qt6 Foundation Plan
status: running
ship: pr-per-milestone
publish_decisions: docs/decisions/
---

# Goal

Fork `NatronGitHub/Natron`, drop Windows/macOS and Qt5, move to C++20 with
current dependencies, and stand up CI/CD that actually gates merges — so
future core work has solid ground to build on.

# Context and constraints

- **No Higx in the repo (user, 2026-11-16):** Higx Optics is the parity reference for traced defocus (M479 - Lens Bokeh Parity Audit), but, like Nuke, it is never named in docs, tooltips, comments or committed files; audits go to the user and PR bodies only.

- **Licence: GPLv3-or-later (user, 2026-11-03).** Natron moves to GPLv3-or-later before release, so Apache-2.0 and GPLv3 code may be integrated (bundled Cycles with Blender's GPL deep files, darktable ports, etc.).

- **Mind the Git LFS quota (user, 2026-10-27):** reference images store only the compared region (≤256 px crops, half-float, zip-compressed); prefer small synthetic sources; no new camera raws without asking.

- **Six lanes, same-file rule waived (user, 2026-10-27):** bosley-native-nodes runs six parallel lanes across E13, E9, E3, E10, E15, E16, E17 plus M224, M163 and M242; lanes may overlap files and accept rebases. M64 - Tiled Rendering gates nothing. A milestone blocked on user feedback pauses and its lane takes the next one.

- **No GPU-host timing gates for placement (user, 2026-11-02):** GPU placement is weight-based (M239); the cost-seed algorithm is gone, so milestones need no per-node GPU-host timing measurements before merge. Drop such gate lines when found. Timings that are a milestone's own deliverable (a perf spike, documented node timings) are unaffected.

- **Never port display-referred nodes (user, 2026-10-30):** darktable and other ports must operate on scene-linear data; a module that only makes sense display-referred is not ported.

- **Features save rebase churn, CI and UAT time (user, 2026-11-05):** a milestone joins a Feature when that cuts rebases, waits on CI and builds, or calls on limited UAT time; it need not be unacceptable alone. A member's gate lines asking for the user (UAT AppImage, screenshots to approve, manual hardware checks) are collected once at the Feature's UAT (§9b step 5), and its automated checks still run at the member's gate. Decisions a member needs in order to continue, such as a spike's mechanism choice, still go to the user as `response`. See `DECISIONS/2026-11-05-feature-grouping.md`.

- **Stop Docker containers gracefully (user, 2026-10-27):** never `docker kill`, `docker rm -f` or SIGKILL a running natron-dev (or any Natron) container; `docker stop` (SIGTERM, a timeout of 30 s or more) and then `docker rm`. Killed containers left GPU/driver memory held on the host, which looked like M225's "memory not released" bug.

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
- **The plan lives on the orphan `plan` branch**, checked out at `.plan/`
  (PLAN-FORMAT.md §1a). Commit code first, then the plan, per §9 — plan edits
  never ride in a code commit or a PR diff. See
  `DECISIONS/2026-08-31-migrate-plan-worktree.md`.
- Grounded in the `RB-2.6` tree (`CMakeLists.txt`, `INSTALL_LINUX.md`,
  `Global/Macros.h`, `tools/jenkins/`), the open PR queue on
  `NatronGitHub/Natron`, and the ASWF `aswf-docker` image catalog, as of
  2026-08-29. Re-check specific line numbers, PR states, and image tags
  before executing — all three will have moved.
- **Structural debts get their own E14 stub** (2026-10-25): when feature work
  exposes a refactor that would widen its diff past scope, file it as a new
  E14 - Architecture stub instead of folding it in. M31 - Architectural Cleanup,
  the old parking place, was triaged and cancelled.

- **Build mode (local vs remote).** Before building, a PM reads this host's
  `.local/build-mode` with `tools/ci/remote/build.sh --mode`. A missing file means
  **remote**, so prefer remote unless the host is powerful. Remote: build with
  `tools/ci/remote/build.sh <fast|release> [--tests]` on pushed HEAD; lanes don't
  share a container, so `--parallel` lanes are fine; run GUI checks locally under
  Xvfb against the downloaded AppImage. Local: `tools/ci/local/build.sh`, one build
  in the container at a time. Either way, batch one build per phase. UAT/GUI checks
  use the `fast` preset; performance work uses the stripped `release` AppImage,
  measured on the target host, never on a GitHub runner.

- **GUI bench rule.** Every milestone whose diff touches `Gui/NodeGraph*`, `Gui/NodeGui*`, `Gui/Edge*`, knob expressions (`Engine/Knob*`) or node naming (`Engine/NodeGroup*`) runs the GUI bench at its gate: `tools/bench/gui/run.sh --xvfb`, then `compare.py` against `tools/bench/gui/baselines/xvfb.jsonl`. It records the result in its `## Decisions`. A regression blocks the gate unless the user accepts it. Xvfb numbers only catch regressions. Budgets are judged on the user's real host. The bench lands with M117 - GUI Perf Harness And Baseline, and the rule applies from then on. See `DECISIONS/2026-10-13-gui-bench-gate-rule.md`.

- **USD nodes pick a toolbar submenu (2026-11-22):** from M520 - USD Toolbar Category And Submenus on, `PLUGIN_GROUP_3D_USD` is gone; a new USD node registers one of the `PLUGIN_GROUP_USD_*` submenus (Prims, Lights, Cameras, Materials, Composition, Edit, Render, I/O), adding a submenu only when none fits. See `DECISIONS/2026-11-22-usd-toolbar-submenus.md`.

- **History:** the sequencing notes, shipped-milestone summaries and backlog-reordering log that used to sit here moved verbatim to `PLAN/ARCHIVE/board-context-history.md` (off the startup path).

# Board

| ID | Milestone | Status | Deps | Owner | Epic | Feature | Needs | File |
|----|-----------|--------|------|-------|------|---|---|------|
| M0 | Fork And Cut Scope | done |  |  | E1 | | | [M0-fork-cut-scope.md](PLAN/MILESTONES/M0-fork-cut-scope.md) |
| M1 | Toolchain Baseline | done |  |  | E1 | | | [M1-toolchain-baseline.md](PLAN/MILESTONES/M1-toolchain-baseline.md) |
| M2 | Qt6 Migration | done |  |  | E1 | | | [M2-qt6-migration.md](PLAN/MILESTONES/M2-qt6-migration.md) |
| M7 | Local Incremental Builds | done |  |  | E2 | | | [M7-local-incremental-builds.md](PLAN/MILESTONES/M7-local-incremental-builds.md) |
| M8 | Branching And CI/CD | done |  |  | E2 | | | [M8-branching-and-cicd.md](PLAN/MILESTONES/M8-branching-and-cicd.md) |
| M9 | ~~Drop Vendored OFX~~ | cancelled |  |  |  | | | [M9-drop-vendored-ofx.md](PLAN/MILESTONES/M9-drop-vendored-ofx.md) |
| M10 | Clean-Sheet CI/CD | done |  |  | E2 | | | [M10-cicd-clean-sheet.md](PLAN/MILESTONES/M10-cicd-clean-sheet.md) |
| M3 | Dependency Modernization | done |  |  | E1 | | | [M3-dependency-modernization.md](PLAN/MILESTONES/M3-dependency-modernization.md) |
| M4 | CI/CD Rebuild | done |  |  | E2 | | | [M4-cicd-rebuild.md](PLAN/MILESTONES/M4-cicd-rebuild.md) |
| M12 | Documentation Cruft Removal | done |  |  | E2 | | | [M12-documentation-cruft-removal.md](PLAN/MILESTONES/M12-documentation-cruft-removal.md) |
| M13 | Full OFX Plugin Set | done |  |  | E1 | | | [M13-full-ofx-plugin-set.md](PLAN/MILESTONES/M13-full-ofx-plugin-set.md) |
| M5 | Test And Correctness Baseline | done |  |  | E2 | | | [M5-test-correctness-baseline.md](PLAN/MILESTONES/M5-test-correctness-baseline.md) |
| M6 | Documentation Pass | done |  |  | E2 | | | [M6-documentation-pass.md](PLAN/MILESTONES/M6-documentation-pass.md) |
| M15 | Release Packaging | done |  |  | E2 | | | [M15-release-packaging.md](PLAN/MILESTONES/M15-release-packaging.md) |
| M11 | OFX Plugin Integration Test | done |  |  | E2 | | | [M11-ofx-plugin-integration-test.md](PLAN/MILESTONES/M11-ofx-plugin-integration-test.md) |
| M14 | Documentation Orphan Branch | done |  |  | E2 | | | [M14-documentation-tree-and-doc-ci.md](PLAN/MILESTONES/M14-documentation-tree-and-doc-ci.md) |
| M17 | Typed Edges Framework | done |  |  | E8 | | | [M17-typed-edges-native-framework.md](PLAN/MILESTONES/M17-typed-edges-native-framework.md) |
| M26 | Test Teardown Flake Fix | done |  |  | E2 | | | [M26-test-fixture-teardown-flake.md](PLAN/MILESTONES/M26-test-fixture-teardown-flake.md) |
| M18 | Deep Compositing v1 | done |  |  | E8 | | | [M18-deep-compositing-v1.md](PLAN/MILESTONES/M18-deep-compositing-v1.md) |
| M23 | Relocatable Release Bundles | done |  |  | E2 | | | [M23-relocatable-release-bundles.md](PLAN/MILESTONES/M23-relocatable-release-bundles.md) |
| M39 | Layers Not Planes | done |  |  | E4 | | | [M39-layers-not-planes.md](PLAN/MILESTONES/M39-layers-not-planes.md) |
| M57 | Write Layer Regressions | done |  |  | E4 | | | [M57-write-node-plane-layer-regressions.md](PLAN/MILESTONES/M57-write-node-plane-layer-regressions.md) |
| M58 | Write Multiplane Pixel Bug | done |  |  | E4 | | | [M58-write-multiplane-pixel-data-bug.md](PLAN/MILESTONES/M58-write-multiplane-pixel-data-bug.md) |
| M59 | Pre-Commit Auto-Format | done |  |  | E2 | | | [M59-pre-commit-auto-format.md](PLAN/MILESTONES/M59-pre-commit-auto-format.md) |
| M38 | Layer Channel Widget | done |  |  | E4 | | | [M38-channel-layer-ui-organization.md](PLAN/MILESTONES/M38-channel-layer-ui-organization.md) |
| M43 | Drop Premult Concept | done | - |  | E4 | | | [M43-drop-premult-concept.md](PLAN/MILESTONES/M43-drop-premult-concept.md) |
| M34 | Native Shuffle Node | done |  |  | E4 | | | [M34-new-native-shuffle-node.md](PLAN/MILESTONES/M34-new-native-shuffle-node.md) |
| M61 | Time-Varying Layers | done |  |  | E4 | | | [M61-layers-that-vary-with-time.md](PLAN/MILESTONES/M61-layers-that-vary-with-time.md) |
| M65 | RGBA Layers | done |  |  | E4 | | | [M65-rgba-rgb-alpha-layers.md](PLAN/MILESTONES/M65-rgba-rgb-alpha-layers.md) |
| M66 | Alpha-Only Plugin Support | done |  |  | E4 | | | [M66-plugin-alpha-only-moderate.md](PLAN/MILESTONES/M66-plugin-alpha-only-moderate.md) |
| M35 | ~~Remove Implicit Shuffle~~ | cancelled |  |  | E4 | | | [M35-remove-implicit-shuffle-elsewhere.md](PLAN/MILESTONES/M35-remove-implicit-shuffle-elsewhere.md) |
| M36 | ~~New Channel Affordance~~ | cancelled |  |  | E4 | | | [M36-new-channel-affordance.md](PLAN/MILESTONES/M36-new-channel-affordance.md) |
| M37 | Channel Management Nodes | done |  |  | E4 | | | [M37-channel-management-nodes.md](PLAN/MILESTONES/M37-channel-management-nodes.md) |
| M50 | Project OCIO Support | done |  |  | E3 | | | [M50-proper-ocio-support.md](PLAN/MILESTONES/M50-proper-ocio-support.md) |
| M60 | Deep Layers And Channels | done |  |  | E8 | | | [M60-deep-layers-and-channels.md](PLAN/MILESTONES/M60-deep-layers-and-channels.md) |
| M62 | Render Scaling Hotspots | done |  |  | E6 | | | [M62-render-scaling-hotspots.md](PLAN/MILESTONES/M62-render-scaling-hotspots.md) |
| M63 | Task-Graph Render Scheduler | done |  |  | E6 | | | [M63-task-graph-render-scheduler.md](PLAN/MILESTONES/M63-task-graph-render-scheduler.md) |
| M67 | Native Core Nodes | done |  |  | E3 | | | [M67-native-core-nodes.md](PLAN/MILESTONES/M67-native-core-nodes.md) |
| M28 | Free RAM Fix | done |  |  | E6 | | | [M28-free-ram-reads-memfree.md](PLAN/MILESTONES/M28-free-ram-reads-memfree.md) |
| M30 | Automated Beta Releases | done |  |  | E2 | | | [M30-automated-beta-releases.md](PLAN/MILESTONES/M30-automated-beta-releases.md) |
| M73 | Native Core Nodes Wrap-Up | done |  |  | E3 | | | [M73-native-core-nodes-wrap-up.md](PLAN/MILESTONES/M73-native-core-nodes-wrap-up.md) |
| M64 | Tiled Rendering | blocked | M63 |  | E6 | | | [M64-tiled-rendering.md](PLAN/MILESTONES/M64-tiled-rendering.md) |
| M74 | Native Metadata Core | done |  |  | E3 | | | [M74-native-metadata-core.md](PLAN/MILESTONES/M74-native-metadata-core.md) |
| M75 | Native Read | done |  |  | E3 | | | [M75-native-read.md](PLAN/MILESTONES/M75-native-read.md) |
| M76 | Native Write | done | M75 |  | E3 | | | [M76-native-write.md](PLAN/MILESTONES/M76-native-write.md) |
| M77 | Native Metadata Nodes | done | M75 | bosley-native-nodes | E3 | | | [M77-native-metadata-nodes.md](PLAN/MILESTONES/M77-native-metadata-nodes.md) |
| M78 | Metadata In Expressions | done | M75, M77 | bosley-native-nodes | E3 | | | [M78-metadata-in-expressions.md](PLAN/MILESTONES/M78-metadata-in-expressions.md) |
| M79 | Layered Document Readers | doing | M75 | bosley-executor | E3 | | | [M79-layered-document-readers.md](PLAN/MILESTONES/M79-layered-document-readers.md) |
| M80 | Native RAW Support | done | M75, M85, M120, M137 | bosley-native-nodes | E3 | | | [M80-native-raw-support.md](PLAN/MILESTONES/M80-native-raw-support.md) |
| M159 | Kind-Agnostic Groups | done | - | bosley-native-nodes | E3 | | | [M159-kind-agnostic-groups.md](PLAN/MILESTONES/M159-kind-agnostic-groups.md) |
| M160 | Lensfun Lens Correction | done | M80, M113 | bosley-native-nodes | E3 | | | [M160-lensfun-lens-correction.md](PLAN/MILESTONES/M160-lensfun-lens-correction.md) |
| M82 | Render Cost Profiling | done |  |  | E6 | | | [M82-render-cost-profiling.md](PLAN/MILESTONES/M82-render-cost-profiling.md) |
| M83 | GPU Compute Backend Spike | done |  |  | E5 | | | [M83-gpu-compute-backend-spike.md](PLAN/MILESTONES/M83-gpu-compute-backend-spike.md) |
| M84 | GPU Placement And Residency | done |  |  | E5 | | | [M84-gpu-placement-and-residency.md](PLAN/MILESTONES/M84-gpu-placement-and-residency.md) |
| M101 | Clean-Sheet Merge Node | done | M84, M137, M158 |  | E3 | | | [M101-clean-sheet-merge-node.md](PLAN/MILESTONES/M101-clean-sheet-merge-node.md) |
| M111 | Native Node Identity Cleanup | done | M76, M101, M120, M184 | bosley-native-nodes | E3 | | | [M111-native-node-identity-cleanup.md](PLAN/MILESTONES/M111-native-node-identity-cleanup.md) |
| M112 | Native Node Behaviour Fixes | done | - | bosley-native-nodes | E3 | | | [M112-native-node-behaviour-fixes.md](PLAN/MILESTONES/M112-native-node-behaviour-fixes.md) |
| M81 | Raw GPU Kernels | done | M80, M85 | gpu | E5 | | | [M81-raw-gpu-kernels.md](PLAN/MILESTONES/M81-raw-gpu-kernels.md) |
| M85 | Native GPU Kernels | done | M84 | gpu | E5 | | | [M85-native-gpu-kernels.md](PLAN/MILESTONES/M85-native-gpu-kernels.md) |
| M107 | GPU Viewer Hand-off | done | M85 | gpu | E5 | | | [M107-gpu-viewer-hand-off.md](PLAN/MILESTONES/M107-gpu-viewer-hand-off.md) |
| M108 | Retire Engine GL Host Path | done | M85 | gpu | E5 | | | [M108-retire-engine-gl-host-path.md](PLAN/MILESTONES/M108-retire-engine-gl-host-path.md) |
| M86 | OFX 1.5 GPU Suites | todo | M85, M108 |  | E5 | | | [M86-ofx-gpu-suites.md](PLAN/MILESTONES/M86-ofx-gpu-suites.md) |
| M87 | ~~Point-Op Kernel Fusion~~ | cancelled | M85, M126 | gpu | E5 | | | [M87-point-op-kernel-fusion.md](PLAN/MILESTONES/M87-point-op-kernel-fusion.md) |
| M109 | GPU Region Engine Hardening | done | M85 | gpu | E5 | | | [M109-gpu-region-engine-hardening.md](PLAN/MILESTONES/M109-gpu-region-engine-hardening.md) |
| M110 | Remaining Native GPU Ports | done | M85, M109, M126, M137 | gpu | E5 | | | [M110-remaining-native-gpu-ports.md](PLAN/MILESTONES/M110-remaining-native-gpu-ports.md) |
| M88 | FIR Gaussian Blur | done |  |  | E6 | | | [M88-fir-gaussian-blur.md](PLAN/MILESTONES/M88-fir-gaussian-blur.md) |
| M89 | Shared Worktree Ccache | done | - |  | E2 | | | [M89-shared-worktree-ccache.md](PLAN/MILESTONES/M89-shared-worktree-ccache.md) |
| M90 | Render Path Serial Overhead | done | M84 | bosley-native-nodes | E6 | | | [M90-render-path-serial-overhead.md](PLAN/MILESTONES/M90-render-path-serial-overhead.md) |
| M91 | NVIDIA GPU Validation | done | M84 | bosley-faux | E5 | | | [M91-nvidia-gpu-validation.md](PLAN/MILESTONES/M91-nvidia-gpu-validation.md) |
| M116 | GPU Region Overlay | done | M81, M85, M91, M107, M108, M109, M110 |  | E5 | | | [M116-gpu-region-overlay.md](PLAN/MILESTONES/M116-gpu-region-overlay.md) |
| M27 | Real Debug Build | done | - | | E2 | | | [M27-debug-build-defines-ndebug.md](PLAN/MILESTONES/M27-debug-build-defines-ndebug.md) |
| M22 | Missing Plugin Placeholder | done | M75 | bosley-native-nodes | E3 | | | [M22-missing-plugin-placeholder.md](PLAN/MILESTONES/M22-missing-plugin-placeholder.md) |
| M31 | ~~Architectural Cleanup~~ | cancelled | M75, M76 |  | E14 | | | [M31-architectural-cleanup.md](PLAN/MILESTONES/M31-architectural-cleanup.md) |
| M69 | Minor Cleanup | done | M44, M55 | amber-falcon | E10 | | | [M69-minor-cleanup.md](PLAN/MILESTONES/M69-minor-cleanup.md) |
| M24 | Node Graph Category Colour | done | - |  | E10 | | | [M24-node-graph-category-colour.md](PLAN/MILESTONES/M24-node-graph-category-colour.md) |
| M44 | Trackball Colour Editing | done | - |  | E10 | | | [M44-trackball-colour-editing.md](PLAN/MILESTONES/M44-trackball-colour-editing.md) |
| M55 | Node Graph Polish | done | M24 |  | E10 | | | [M55-polish-node-graph-interaction.md](PLAN/MILESTONES/M55-polish-node-graph-interaction.md) |
| M46 | Labelmaker Annotations | blocked | M56, M118 |  | E10 | | | [M46-labelmaker-annotations.md](PLAN/MILESTONES/M46-labelmaker-annotations.md) |
| M56 | Visual And Menu Polish | done | M44, M55 | amber-falcon | E10 | | | [M56-polish-visual-and-menus.md](PLAN/MILESTONES/M56-polish-visual-and-menus.md) |
| M45 | Native Tab Menu | done | M55 | amber-falcon | E10 | | | [M45-tabtabtab-native-tab-menu.md](PLAN/MILESTONES/M45-tabtabtab-native-tab-menu.md) |
| M25 | GL Init FP Guard | done |  | bosley-native-nodes | E2 | | | [M25-debug-fp-trap-gl-init.md](PLAN/MILESTONES/M25-debug-fp-trap-gl-init.md) |
| M29 | ~~Independent Repository~~ | cancelled |  |  | E2 | | | [M29-independent-repository.md](PLAN/MILESTONES/M29-independent-repository.md) |
| M68 | Headless GL EGL | todo | M20, M63, M108 |  | E2 | | | [M68-headless-gl-egl-backend.md](PLAN/MILESTONES/M68-headless-gl-egl-backend.md) |
| M19 | USD/Hydra Foundation | done | M17, M161, M107 |  | E9 | | | [M19-usd-hydra-foundation.md](PLAN/MILESTONES/M19-usd-hydra-foundation.md) |
| M20 | 3D Node Vocabulary | done | M19 | bosley-native-nodes | E9 | | | [M20-3d-node-vocabulary.md](PLAN/MILESTONES/M20-3d-node-vocabulary.md) |
| M226 | Scene Hierarchy From Topology | done | M19, M20 |  | E9 | | | [M226-scene-hierarchy-from-topology.md](PLAN/MILESTONES/M226-scene-hierarchy-from-topology.md) |
| M227 | USD Primitive Nodes | done | M226 | bosley-native-nodes | E9 | | | [M227-usd-primitive-nodes.md](PLAN/MILESTONES/M227-usd-primitive-nodes.md) |
| M228 | 3D Viewer Navigation | done | M19, M20, M226 | bosley-native-nodes | E9 | | | [M228-3d-viewer-navigation.md](PLAN/MILESTONES/M228-3d-viewer-navigation.md) |
| M229 | 3D Viewer Guides And Display | done | M228 | bosley-native-nodes | E9 | | | [M229-3d-viewer-guides-and-display.md](PLAN/MILESTONES/M229-3d-viewer-guides-and-display.md) |
| M230 | 3D Transform Widgets | done | M226, M229 | bosley-native-nodes | E9 | | | [M230-3d-transform-widgets.md](PLAN/MILESTONES/M230-3d-transform-widgets.md) |
| M231 | Scene Graph Panel | done | M229 | bosley-native-nodes | E9 | | | [M231-scene-graph-panel.md](PLAN/MILESTONES/M231-scene-graph-panel.md) |
| M232 | Scene Selection To Nodes | done | M226, M231 | bosley-native-nodes | E9 | | | [M232-scene-selection-to-nodes.md](PLAN/MILESTONES/M232-scene-selection-to-nodes.md) |
| M327 | Heavy Scene Spike | done | M231, M232 | misty-marten | E9 | | | [M327-heavy-scene-spike.md](PLAN/MILESTONES/M327-heavy-scene-spike.md) |
| M328 | Deferred Viewer Stage | done | M327 | | E9 | F4 | | [M328-deferred-viewer-stage.md](PLAN/MILESTONES/M328-deferred-viewer-stage.md) |
| M329 | Viewer Draw States | done | M327, M328 | bosley-native-nodes | E9 | F4 | | [M329-viewer-draw-states.md](PLAN/MILESTONES/M329-viewer-draw-states.md) |
| M330 | Graph Draw Policy | done | M329 | bosley-native-nodes | E9 | F4 | | [M330-graph-draw-policy.md](PLAN/MILESTONES/M330-graph-draw-policy.md) |
| M331 | Deferred Pattern Matching | done | M52, M327, M328 | bosley-native-nodes | E9 | F4 | | [M331-deferred-pattern-matching.md](PLAN/MILESTONES/M331-deferred-pattern-matching.md) |
| M332 | Scene Graph Points | done | M328 | bosley-native-nodes | E9 | F4 | | [M332-scene-graph-points.md](PLAN/MILESTONES/M332-scene-graph-points.md) |
| M333 | Prim Inspector | done | M232 | misty-marten | E9 | F4 | | [M333-prim-inspector.md](PLAN/MILESTONES/M333-prim-inspector.md) |
| M334 | Knob Prim Tools | done | M52, M232, M331 | bosley-native-nodes | E9 | F4 | | [M334-knob-prim-tools.md](PLAN/MILESTONES/M334-knob-prim-tools.md) |
| M335 | Viewer Render Draw States | done | M254, M329 | bosley-native-nodes | E9 | F4 | | [M335-viewer-render-draw-states.md](PLAN/MILESTONES/M335-viewer-render-draw-states.md) |
| M336 | Heavy Scene Acceptance | done | M329, M330, M331, M332 | | E9 | F4 | | [M336-heavy-scene-acceptance.md](PLAN/MILESTONES/M336-heavy-scene-acceptance.md) |
| M401 | 3D Viewer Breadcrumbs | done | M231, M232, M328, M332 | bosley-native-nodes | E9 | F4 | | [M401-3d-viewer-breadcrumbs.md](PLAN/MILESTONES/M401-3d-viewer-breadcrumbs.md) |
| M481 | Viewer Polygon Budgets | doing | M336 | bosley-executor | E9 | | uat | [M481-viewer-polygon-budgets.md](PLAN/MILESTONES/M481-viewer-polygon-budgets.md) |
| M423 | USDPrune Isolate Mode | done | M381 | | E9 | | | [M423-usdprune-isolate-mode.md](PLAN/MILESTONES/M423-usdprune-isolate-mode.md) |
| M337 | Geometry Spreadsheet | done | M54, M358, M361, M333 | | E9 | F15 | | [M337-geometry-spreadsheet.md](PLAN/MILESTONES/M337-geometry-spreadsheet.md) |
| M233 | Live Node Textures | done | M19, M20 | bosley-native-nodes | E9 | | | [M233-live-node-textures.md](PLAN/MILESTONES/M233-live-node-textures.md) |
| M234 | Camera Projection Nodes | done | M227, M233 | bosley-native-nodes | E9 | | | [M234-camera-projection-nodes.md](PLAN/MILESTONES/M234-camera-projection-nodes.md) |
| M235 | USDCamera From File | done | M226 | bosley-native-nodes | E9 | | | [M235-usdcamera-from-file.md](PLAN/MILESTONES/M235-usdcamera-from-file.md) |
| M236 | Shared Materials And Assignment | done | M226 | bosley-native-nodes | E9 | | | [M236-shared-materials-and-assignment.md](PLAN/MILESTONES/M236-shared-materials-and-assignment.md) |
| M237 | CI Flake Fixes | done | - | bosley-native-nodes | E2 | | | [M237-ci-flake-fixes.md](PLAN/MILESTONES/M237-ci-flake-fixes.md) |
| M238 | EdgePush And Edge Fill Kit | todo | M123 |  | E3 | F39 | | [M238-edgeextend-options.md](PLAN/MILESTONES/M238-edgeextend-options.md) |
| M239 | Weight-Based GPU Placement | done | - | gpu | E5 | | | [M239-weight-based-gpu-placement.md](PLAN/MILESTONES/M239-weight-based-gpu-placement.md) |
| M240 | Float Trap And Denormal Audit | done | - | gpu | E14 | | | [M240-float-trap-and-denormal-audit.md](PLAN/MILESTONES/M240-float-trap-and-denormal-audit.md) |
| M241 | Up Axis And Units Policy | done | M226 |  | E9 | | | [M241-up-axis-policy.md](PLAN/MILESTONES/M241-up-axis-policy.md) |
| M242 | Cache Invalidation Audit | done | M178, M185 | bosley-native-nodes | E14 | | | [M242-cache-invalidation-audit.md](PLAN/MILESTONES/M242-cache-invalidation-audit.md) |
| M243 | Startup Pane Paint On NVIDIA | todo | - |  | E10 | | | [M243-startup-pane-paint-on-nvidia.md](PLAN/MILESTONES/M243-startup-pane-paint-on-nvidia.md) |
| M244 | NVIDIA Test Fixes | todo | M239 |  | E5 | | | [M244-nvidia-test-fixes.md](PLAN/MILESTONES/M244-nvidia-test-fixes.md) |
| M245 | Write Path Variables | done | - | bosley-native-nodes | E3 | | | [M245-write-path-variables.md](PLAN/MILESTONES/M245-write-path-variables.md) |
| M246 | Vulkan Inference Provider | todo | M203, M204 | | E16 | | | [M246-vulkan-inference-provider.md](PLAN/MILESTONES/M246-vulkan-inference-provider.md) |
| M247 | Project User Knobs And Root | todo | M16, M373 |  | E14 | | | [M247-project-user-knobs-and-root.md](PLAN/MILESTONES/M247-project-user-knobs-and-root.md) |
| M248 | Render Delegate Selection | done | M20 |  | E9 | | | [M248-render-delegate-selection.md](PLAN/MILESTONES/M248-render-delegate-selection.md) |
| M249 | Bundle Cycles Delegate | done | M19, M248 | misty-marten | E9 | F3 | | [M249-bundle-cycles-delegate.md](PLAN/MILESTONES/M249-bundle-cycles-delegate.md) |
| M250 | Bundle MoonRay Delegate | done | M19 | misty-marten | E9 | F3 | | [M250-bundle-moonray-delegate.md](PLAN/MILESTONES/M250-bundle-moonray-delegate.md) |
| M251 | Cycles Rendering | done | M248, M249 | misty-marten | E9 | F3 | | [M251-cycles-rendering.md](PLAN/MILESTONES/M251-cycles-rendering.md) |
| M252 | MoonRay Rendering | done | M248, M250 | misty-marten | E9 | F3 | | [M252-moonray-rendering.md](PLAN/MILESTONES/M252-moonray-rendering.md) |
| M253 | Delegate Render Settings | done | M251, M252 | misty-marten | E9 | F3 | | [M253-delegate-render-settings.md](PLAN/MILESTONES/M253-delegate-render-settings.md) |
| M254 | Progressive Viewer Render | done | M248, M251 | misty-marten | E9 | F3 | | [M254-progressive-viewer-render.md](PLAN/MILESTONES/M254-progressive-viewer-render.md) |
| M255 | Delegate Output Spikes | done | M251, M252 | | E9 | | | [M255-delegate-output-spikes.md](PLAN/MILESTONES/M255-delegate-output-spikes.md) |
| M256 | Delegate AOV Selection | doing | M255 | bosley-executor | E9 | | | [M256-delegate-aov-selection.md](PLAN/MILESTONES/M256-delegate-aov-selection.md) |
| M257 | Cycles Deep Output | todo | M255, M286 |  | E8 | F24 | | [M257-cycles-deep-output.md](PLAN/MILESTONES/M257-cycles-deep-output.md) |
| M460 | MoonRay True Deep Patch | doing | M255 | bosley-executor | E8 | F24 | | [M460-moonray-true-deep-patch.md](PLAN/MILESTONES/M460-moonray-true-deep-patch.md) |
| M461 | MoonRay DeepFrame Over Arras | todo | M255, M286 |  | E8 | F24 | | [M461-moonray-deepframe-over-arras.md](PLAN/MILESTONES/M461-moonray-deepframe-over-arras.md) |
| M258 | MoonRay Deep Output | todo | M255, M286, M460, M461 |  | E8 | F24 | | [M258-moonray-deep-output.md](PLAN/MILESTONES/M258-moonray-deep-output.md) |
| M259 | GPU Placement Under Render Contexts | done | M180, M181, M182 | bosley-native-nodes | E15 | | | [M259-gpu-placement-under-render-contexts.md](PLAN/MILESTONES/M259-gpu-placement-under-render-contexts.md) |
| M52 | USD Layer-Stack Completion | done | M226 | bosley-native-nodes | E9 | | | [M52-usd-layer-stack-completion.md](PLAN/MILESTONES/M52-usd-layer-stack-completion.md) |
| M53 | Projected Texture Painting | done | M52, M233, M311 | bosley-native-nodes | E9 | F11 | | [M53-projected-texture-painting.md](PLAN/MILESTONES/M53-projected-texture-painting.md) |
| M54 | Native Geometry Data Model | done | M52, M227 | bosley-native-nodes | E9 | F15 | | [M54-sops-style-geometry.md](PLAN/MILESTONES/M54-sops-style-geometry.md) |
| M358 | Geometry Bridges And Display | done | M54, M52 | | E9 | F15 | | [M358-geometry-bridges-and-display.md](PLAN/MILESTONES/M358-geometry-bridges-and-display.md) |
| M359 | Geometry Operators | done | M54 | | E9 | F15 | | [M359-geometry-operators.md](PLAN/MILESTONES/M359-geometry-operators.md) |
| M360 | Geometry Kernel Language | done | M54 | | E9 | F15 | | [M360-geometry-kernel-language.md](PLAN/MILESTONES/M360-geometry-kernel-language.md) |
| M361 | Geometry Point Editing | done | M358, M359, M360, M376 | | E9 | F15 | | [M361-geometry-point-editing.md](PLAN/MILESTONES/M361-geometry-point-editing.md) |
| M362 | Geometry Operators II | todo | M359, M360 |  | E9 | F21 | | [M362-geometry-operators-ii.md](PLAN/MILESTONES/M362-geometry-operators-ii.md) |
| M363 | Geometry Display Extras | todo | M358, M376, M607 |  | E9 |  | | [M363-geometry-display-extras.md](PLAN/MILESTONES/M363-geometry-display-extras.md) |
| M402 | Geometry Loops | done | M180, M181, M190, M359, M360 |  | E22 | F18 | | [M402-geometry-loops.md](PLAN/MILESTONES/M402-geometry-loops.md) |
| M403 | Geometry File Cache | done | M54, M359 |  | E22 | F18 | | [M403-geometry-file-cache.md](PLAN/MILESTONES/M403-geometry-file-cache.md) |
| M404 | Geometry Solver | doing | M402, M403 | bosley-executor | E22 | F18 | | [M404-geometry-solver.md](PLAN/MILESTONES/M404-geometry-solver.md) |
| M405 | Particle Solver Core | todo | M404, M359 |  | E22 | F19 | | [M405-particle-solver-core.md](PLAN/MILESTONES/M405-particle-solver-core.md) |
| M406 | Particle Forces And Wrangle | todo | M405, M360 |  | E22 | F19 | | [M406-particle-forces-and-wrangle.md](PLAN/MILESTONES/M406-particle-forces-and-wrangle.md) |
| M407 | Particle Collisions | todo | M405, M418 |  | E22 | F19 | | [M407-particle-collisions.md](PLAN/MILESTONES/M407-particle-collisions.md) |
| M408 | Kernel Language GPU Backend | done | M360 | | E22 | F19 | | [M408-kernel-language-gpu-backend.md](PLAN/MILESTONES/M408-kernel-language-gpu-backend.md) |
| M409 | Particle GPU Path | todo | M406, M407, M408 |  | E22 | F19 | | [M409-particle-gpu-path.md](PLAN/MILESTONES/M409-particle-gpu-path.md) |
| M412 | 2D Image Particles | todo | M405, M406 |  | E22 | F19 | | [M412-2d-image-particles.md](PLAN/MILESTONES/M412-2d-image-particles.md) |
| M410 | 2D Fluid Solver Core | todo | M190 |  | E22 | F20 | | [M410-2d-fluid-solver-core.md](PLAN/MILESTONES/M410-2d-fluid-solver-core.md) |
| M411 | 2D Fluid Fire And Controls | todo | M410 |  | E22 | F20 | | [M411-2d-fluid-fire-and-controls.md](PLAN/MILESTONES/M411-2d-fluid-fire-and-controls.md) |
| M420 | Spline Curve Primitive | todo | M358, M376 |  | E9 | F21 | | [M420-spline-curve-primitive.md](PLAN/MILESTONES/M420-spline-curve-primitive.md) |
| M421 | Stroke Keyframe Indicators | done | M261, M305, M272 | | E10 | | | [M421-stroke-keyframe-indicators.md](PLAN/MILESTONES/M421-stroke-keyframe-indicators.md) |
| M424 | Python setExpression Refreshes The Viewer | done | - | bosley-native-nodes | E10 | | | [M424-python-set-expression-refreshes-viewer.md](PLAN/MILESTONES/M424-python-set-expression-refreshes-viewer.md) |
| M422 | Snap Viewer Wheel Zoom | done | - | | E10 |  F29 | | [M422-snap-viewer-wheel-zoom.md](PLAN/MILESTONES/M422-snap-viewer-wheel-zoom.md) |
| M413 | Geometry Modeling Ops | todo | M359, M420 |  | E9 | F21 | | [M413-geometry-modeling-ops.md](PLAN/MILESTONES/M413-geometry-modeling-ops.md) |
| M415 | Geometry Attribute Ops | todo | M359, M360, M402 |  | E9 | F21 | | [M415-geometry-attribute-ops.md](PLAN/MILESTONES/M415-geometry-attribute-ops.md) |
| M414 | Geometry Advanced Modeling | todo | M359, M415 |  | E9 | F21 | | [M414-geometry-advanced-modeling.md](PLAN/MILESTONES/M414-geometry-advanced-modeling.md) |
| M416 | Geometry Deformers And Time | todo | M359, M415 |  | E9 | F21 | | [M416-geometry-deformers-and-time.md](PLAN/MILESTONES/M416-geometry-deformers-and-time.md) |
| M417 | VDB Volume Primitive | doing | M358 | bosley-executor | E9 | F22 | | [M417-vdb-volume-primitive.md](PLAN/MILESTONES/M417-vdb-volume-primitive.md) |
| M418 | VDB Conversion Ops | doing | M417 | bosley-executor | E9 | F22 | | [M418-vdb-conversion-ops.md](PLAN/MILESTONES/M418-vdb-conversion-ops.md) |
| M419 | VDB SDF Ops And Wrangle | todo | M418, M360 |  | E9 | F22 | | [M419-vdb-sdf-ops-and-wrangle.md](PLAN/MILESTONES/M419-vdb-sdf-ops-and-wrangle.md) |
| M376 | Geometry Display | done | M328, M358 | | E9 | F15 | | [M376-geometry-display.md](PLAN/MILESTONES/M376-geometry-display.md) |
| M377 | Readable CI Names | done | - | bosley-native-nodes | E2 | | | [M377-readable-ci-names.md](PLAN/MILESTONES/M377-readable-ci-names.md) |
| M378 | Scene Render Test Stability | done | - | bosley-native-nodes | E9 | | | [M378-scene-render-test-stability.md](PLAN/MILESTONES/M378-scene-render-test-stability.md) |
| M379 | GpuContext Test Segfault | done | - | bosley-native-nodes | E5 | | | [M379-gpucontext-test-segfault.md](PLAN/MILESTONES/M379-gpucontext-test-segfault.md) |
| M380 | Blocking Scheduler Aborts | done | - | bosley-native-nodes | E14 | | | [M380-blocking-scheduler-aborts.md](PLAN/MILESTONES/M380-blocking-scheduler-aborts.md) |
| M381 | Deferred Matching Coverage | done | M331 | bosley-native-nodes | E9 | F4 | | [M381-deferred-matching-coverage.md](PLAN/MILESTONES/M381-deferred-matching-coverage.md) |
| M382 | Nucleon Logo And Artwork | doing | - | bosley-nucleon-art-design | E20 | F17 | response | [M382-nucleon-logo-and-artwork.md](PLAN/MILESTONES/M382-nucleon-logo-and-artwork.md) |
| M383 | Rename Script And Guard | todo | - |  | E20 | F17 | | [M383-rename-script-and-guard.md](PLAN/MILESTONES/M383-rename-script-and-guard.md) |
| M384 | Plugin Forks Rename | todo | M383 |  | E20 | F17 | | [M384-plugin-forks-rename.md](PLAN/MILESTONES/M384-plugin-forks-rename.md) |
| M385 | OpenNucleon Code Rename | todo | M157, M383, M384 |  | E20 | F17 | | [M385-opennucleon-code-rename.md](PLAN/MILESTONES/M385-opennucleon-code-rename.md) |
| M386 | Nucleon Branding And Packaging | todo | M382, M385 |  | E20 | F17 | | [M386-nucleon-branding-and-packaging.md](PLAN/MILESTONES/M386-nucleon-branding-and-packaging.md) |
| M387 | Dev Infrastructure Rename | todo | M385 |  | E20 | F17 | | [M387-dev-infrastructure-rename.md](PLAN/MILESTONES/M387-dev-infrastructure-rename.md) |
| M388 | Docs Site Scaffold | todo | - |  | E21 | | | [M388-docs-site-scaffold.md](PLAN/MILESTONES/M388-docs-site-scaffold.md) |
| M389 | Node Reference Export | todo | M388 |  | E21 | | | [M389-node-reference-export.md](PLAN/MILESTONES/M389-node-reference-export.md) |
| M390 | Python API Reference | todo | M388 |  | E21 | | | [M390-python-api-reference.md](PLAN/MILESTONES/M390-python-api-reference.md) |
| M391 | In-App Docs Bundle | todo | M386, M389, M390 |  | E21 | | | [M391-in-app-docs-bundle.md](PLAN/MILESTONES/M391-in-app-docs-bundle.md) |
| M392 | Docs Publishing | todo | M387, M389, M390, M394 |  | E21 | | | [M392-docs-publishing.md](PLAN/MILESTONES/M392-docs-publishing.md) |
| M393 | Landing Site | todo | M382, M388 |  | E21 | | | [M393-landing-site.md](PLAN/MILESTONES/M393-landing-site.md) |
| M394 | Release Versioning | todo | M386, M387 |  | E21 | | | [M394-release-versioning.md](PLAN/MILESTONES/M394-release-versioning.md) |
| M395 | Release Notes 1.0 | todo | M388, M394 |  | E21 | | | [M395-release-notes-1-0.md](PLAN/MILESTONES/M395-release-notes-1-0.md) |
| M396 | Release 1.0 Cut | todo | M391, M392, M393, M394, M395 |  | E21 | | | [M396-release-1-0-cut.md](PLAN/MILESTONES/M396-release-1-0-cut.md) |
| M397 | Label Resize Keeps Node Top-Centre | done | - | | E10 | | | [M397-label-resize-keeps-node-top-centre.md](PLAN/MILESTONES/M397-label-resize-keeps-node-top-centre.md) |
| M398 | Cycles Offline Render After Scene Edits | done | - | bosley-native-nodes | E9 | |  | [M398-cycles-offline-render-after-scene-edits.md](PLAN/MILESTONES/M398-cycles-offline-render-after-scene-edits.md) |
| M399 | Node Creation Keys And Speed | done | M118 | bosley-native-nodes | E10 | | | [M399-node-creation-keys-and-speed.md](PLAN/MILESTONES/M399-node-creation-keys-and-speed.md) |
| M21 | Deep Tier-2 Nodes | todo | M18, M60 |  | E8 | F23 | | [M21-deep-tier2-and-bridges.md](PLAN/MILESTONES/M21-deep-tier2-and-bridges.md) |
| M51 | Deep Filtering OpenDCX | todo | M18, M60 |  | E8 | F23 | | [M51-deep-filtering-opendcx.md](PLAN/MILESTONES/M51-deep-filtering-opendcx.md) |
| M32 | Deep Sample Node | todo | M18, M60 |  | E8 | F23 | | [M32-deep-sample-node.md](PLAN/MILESTONES/M32-deep-sample-node.md) |
| M93 | Serialization Boundary | done | - | slate-ferret | E7 | | | [M93-serialization-boundary.md](PLAN/MILESTONES/M93-serialization-boundary.md) |
| M100 | Content Node Hashing | done | M93 | bosley-native-nodes | E7 | | | [M100-content-node-hashing.md](PLAN/MILESTONES/M100-content-node-hashing.md) |
| M16 | Project Format Redesign | done | M93 | bosley-native-nodes | E7 | | | [M16-project-file-format-redesign.md](PLAN/MILESTONES/M16-project-file-format-redesign.md) |
| M114 | Global Undo Correctness | done | M93 | bosley-native-nodes | E7 | | | [M114-global-undo-correctness.md](PLAN/MILESTONES/M114-global-undo-correctness.md) |
| M102 | Graph Edit Operations | done | M16 | bosley-native-nodes | E7 | | | [M102-graph-edit-operations.md](PLAN/MILESTONES/M102-graph-edit-operations.md) |
| M103 | Graph Diff And Merge | done | M102 |  | E12 | F33 | | [M103-graph-diff-and-merge.md](PLAN/MILESTONES/M103-graph-diff-and-merge.md) |
| M104 | Merge Resolution UI | done | M103 | | E12 | F33 | | [M104-merge-resolution-ui.md](PLAN/MILESTONES/M104-merge-resolution-ui.md) |
| M105 | Live Session Core | done | M102 | | E11 | F5 | | [M105-live-session-core.md](PLAN/MILESTONES/M105-live-session-core.md) |
| M106 | Live Session Presence And Undo | done | M105 | | E11 | F5 | | [M106-live-session-presence-and-undo.md](PLAN/MILESTONES/M106-live-session-presence-and-undo.md) |
| M70 | RFV Declaration And Storage | done |  |  | E13 | | | [M70-rfv-declaration-and-storage.md](PLAN/MILESTONES/M70-rfv-declaration-and-storage.md) |
| M71 | RFV Render Context | done | M70, M178 |  | E13 | | | [M71-rfv-render-context.md](PLAN/MILESTONES/M71-rfv-render-context.md) |
| M72 | RFV Integration And UAT | done |  | bosley-native-nodes | E13 | | | [M72-rfv-integration-and-uat.md](PLAN/MILESTONES/M72-rfv-integration-and-uat.md) |
| M92 | Linked Nodes | done | M24 | bosley-native-nodes | E10 | | | [M92-linked-nodes.md](PLAN/MILESTONES/M92-linked-nodes.md) |
| M95 | Knob Cleanup And Keyer Redesign | done | M69, M76, M137 | bosley-native-nodes | E3 | | | [M95-knob-cleanup-and-keyer-redesign.md](PLAN/MILESTONES/M95-knob-cleanup-and-keyer-redesign.md) |
| M96 | Weighted Curve Tangents | done | M93, M16 | bosley-native-nodes | E7 | | | [M96-weighted-curve-tangents.md](PLAN/MILESTONES/M96-weighted-curve-tangents.md) |
| M97 | ~~Value-Based Node Hashing~~ | cancelled |  |  | E7 | | | [M97-value-based-node-hashing.md](PLAN/MILESTONES/M97-value-based-node-hashing.md) |
| M94 | Delete Unused Maintainer Skill | done | - | bosley-native-nodes | E2 | | | [M94-delete-unused-maintainer-skill.md](PLAN/MILESTONES/M94-delete-unused-maintainer-skill.md) |
| M115 | Build Speed And Artifact Size | done | - | bosley-native-nodes | E2 | | | [M115-build-speed-and-artifact-size.md](PLAN/MILESTONES/M115-build-speed-and-artifact-size.md) |
| M113 | Native Warp Nodes | done | M120, M56 | bosley-native-nodes | E3 | | | [M113-native-node-rewrites-continued.md](PLAN/MILESTONES/M113-native-node-rewrites-continued.md) |
| M120 | Native Node Port Kit | done | M85, M137 | bosley-native-nodes | E3 | | | [M120-native-node-port-kit.md](PLAN/MILESTONES/M120-native-node-port-kit.md) |
| M121 | Native Routing And Mix | done | M120, M101, M137 | bosley-native-nodes | E3 | | | [M121-native-routing-and-mix.md](PLAN/MILESTONES/M121-native-routing-and-mix.md) |
| M122 | Native Colour Utilities | done | M120, M137 |  | E3 | | | [M122-native-colour-utilities.md](PLAN/MILESTONES/M122-native-colour-utilities.md) |
| M123 | Native Sharpen And Glow | done | M120 |  | E3 | | | [M123-native-sharpen-and-glow.md](PLAN/MILESTONES/M123-native-sharpen-and-glow.md) |
| M124 | Native Time Remaps | done | M120 | bosley-native-nodes | E3 | | | [M124-native-time-remaps.md](PLAN/MILESTONES/M124-native-time-remaps.md) |
| M125 | Native Multi-Frame Nodes | done | M124 | bosley-native-nodes | E3 | | | [M125-native-multi-frame-nodes.md](PLAN/MILESTONES/M125-native-multi-frame-nodes.md) |
| M126 | Runtime GPU Shaders | done | M85 | gpu | E5 | | | [M126-runtime-gpu-shaders.md](PLAN/MILESTONES/M126-runtime-gpu-shaders.md) |
| M127 | Native OCIO Nodes | done | M120, M126, M107 |  | E3 | | | [M127-native-ocio-nodes.md](PLAN/MILESTONES/M127-native-ocio-nodes.md) |
| M128 | Native Shape Generators | done | M120 | bosley-native-nodes | E3 | | | [M128-native-shape-generators.md](PLAN/MILESTONES/M128-native-shape-generators.md) |
| M129 | Clean-Sheet Noise Node | done | M120 |  | E3 | | | [M129-clean-sheet-noise-node.md](PLAN/MILESTONES/M129-clean-sheet-noise-node.md) |
| M130 | Native Edge-Preserving And Directional Filters | done | M120 | bosley-native-nodes | E3 | |  | [M130-native-edge-preserving-and-directional-filters.md](PLAN/MILESTONES/M130-native-edge-preserving-and-directional-filters.md) |
| M131 | Native Defocus Family | done | M120 |  | E3 | | | [M131-native-defocus-family.md](PLAN/MILESTONES/M131-native-defocus-family.md) |
| M132 | Native Lens Distortion | done | M113 |  | E3 | | | [M132-native-lens-distortion.md](PLAN/MILESTONES/M132-native-lens-distortion.md) |
| M133 | ZDefocus Lens Aberrations | done | M131 | bosley-native-nodes | E3 | | | [M133-zdefocus-lens-aberrations.md](PLAN/MILESTONES/M133-zdefocus-lens-aberrations.md) |
| M117 | GUI Perf Harness And Baseline | done | - | bosley-native-nodes | E10 | | | [M117-gui-perf-harness-and-baseline.md](PLAN/MILESTONES/M117-gui-perf-harness-and-baseline.md) |
| M118 | Large Graph GUI Responsiveness | done | M117, M56 |  | E10 | | | [M118-large-graph-gui-responsiveness.md](PLAN/MILESTONES/M118-large-graph-gui-responsiveness.md) |
| M119 | Trackball Panel Layout | done | M56, M69 | bosley-native-nodes | E10 | | | [M119-trackball-panel-layout.md](PLAN/MILESTONES/M119-trackball-panel-layout.md) |
| M134 | Output Picker Pixel Aspect | done | - | bosley-native-nodes | E10 | | | [M134-output-picker-pixel-aspect.md](PLAN/MILESTONES/M134-output-picker-pixel-aspect.md) |
| M135 | Flaky Test Fixes | done | - | bosley-native-nodes | E2 | | | [M135-pool-sharing-flake-fix.md](PLAN/MILESTONES/M135-pool-sharing-flake-fix.md) |
| M136 | Edge Policy Investigation | done | M167 |  | E3 | |  | [M136-edge-policy-investigation.md](PLAN/MILESTONES/M136-edge-policy-investigation.md) |
| M137 | Native Base Class Flexibility | done | - | bosley-native-nodes | E3 | | | [M137-native-base-class-flexibility.md](PLAN/MILESTONES/M137-native-base-class-flexibility.md) |
| M138 | GUI Papercuts | done | - | bosley-native-nodes | E10 | | | [M138-gui-papercuts.md](PLAN/MILESTONES/M138-gui-papercuts.md) |
| M139 | Blur In-Graph Overhead | todo | M90, M137 |  | E6 | | | [M139-blur-in-graph-overhead.md](PLAN/MILESTONES/M139-blur-in-graph-overhead.md) |
| M140 | Native Metadata Utilities | done | M77, M120 | misty-marten | E3 | | | [M140-native-metadata-utilities.md](PLAN/MILESTONES/M140-native-metadata-utilities.md) |
| M141 | Complete Local Build Environment | done | - | bosley-native-nodes | E2 | | | [M141-complete-local-build-environment.md](PLAN/MILESTONES/M141-complete-local-build-environment.md) |
| M142 | Atomic Project Save | done | - | bosley-native-nodes | E7 | | | [M142-atomic-project-save.md](PLAN/MILESTONES/M142-atomic-project-save.md) |
| M143 | Bundled Plugin Bug Fixes | done | M100 | bosley-native-nodes | E3 | | | [M143-bundled-plugin-bug-fixes.md](PLAN/MILESTONES/M143-bundled-plugin-bug-fixes.md) |
| M144 | ~~Render-Thread Metadata Refresh~~ | cancelled | M100 |  | E14 | | | [M144-render-thread-metadata-refresh.md](PLAN/MILESTONES/M144-render-thread-metadata-refresh.md) |
| M145 | OFX Fork Plugin Purge | todo | M113, M121, M122, M123, M124, M125, M127, M128, M129, M130, M132, M140, M143 |  | E3 | | | [M145-ofx-fork-plugin-purge.md](PLAN/MILESTONES/M145-ofx-fork-plugin-purge.md) |
| M146 | Roto Hosting For Any Node | done | M137 | bosley-native-nodes | E14 | | | [M146-roto-hosting-for-any-node.md](PLAN/MILESTONES/M146-roto-hosting-for-any-node.md) |
| M147 | SplineWarp Node Core | done | M146, M113, M120, M137 | bosley-native-nodes | E3 | | | [M147-splinewarp-node-core.md](PLAN/MILESTONES/M147-splinewarp-node-core.md) |
| M148 | SplineWarp Viewer Tools | done | M147 | bosley-native-nodes | E3 | | | [M148-splinewarp-viewer-tools.md](PLAN/MILESTONES/M148-splinewarp-viewer-tools.md) |
| M149 | SplineWarp Mesh Solver | done | M147 | bosley-native-nodes | E3 | | | [M149-splinewarp-mesh-solver.md](PLAN/MILESTONES/M149-splinewarp-mesh-solver.md) |
| M150 | SplineWarp GPU Kernel | done | M147 | bosley-native-nodes | E3 | | | [M150-splinewarp-gpu-kernel.md](PLAN/MILESTONES/M150-splinewarp-gpu-kernel.md) |
| M151 | SplineWarp Motion Blur | done | M147, M150 | bosley-native-nodes | E3 | | | [M151-splinewarp-motion-blur.md](PLAN/MILESTONES/M151-splinewarp-motion-blur.md) |
| M152 | Curve Track Links And Roto Copy | done | M148 | bosley-native-nodes | E3 | | | [M152-curve-track-links-and-roto-copy.md](PLAN/MILESTONES/M152-curve-track-links-and-roto-copy.md) |
| M153 | Distortion Concatenation | done | M147, M113, M433, M179 |  | E14 | F25 | | [M153-distortion-concatenation.md](PLAN/MILESTONES/M153-distortion-concatenation.md) |
| M154 | Fresh Buffer Page Zeroing | done | M109 | gpu | E5 | | | [M154-fresh-buffer-page-zeroing.md](PLAN/MILESTONES/M154-fresh-buffer-page-zeroing.md) |
| M155 | Per-Suite Test Processes | done | - | bosley-native-nodes | E2 | | | [M155-per-suite-test-processes.md](PLAN/MILESTONES/M155-per-suite-test-processes.md) |
| M156 | GPU Download Throughput In Comps | done | M109 | gpu | E5 | | | [M156-gpu-download-throughput-in-comps.md](PLAN/MILESTONES/M156-gpu-download-throughput-in-comps.md) |
| M157 | Relicense To GPLv3 Or Later | todo | - |  | E2 | F17 | | [M157-relicense-to-gplv3-or-later.md](PLAN/MILESTONES/M157-relicense-to-gplv3-or-later.md) |
| M158 | Node Toolbar As Menu | done | - | bosley-native-nodes | E10 | | | [M158-node-toolbar-as-menu.md](PLAN/MILESTONES/M158-node-toolbar-as-menu.md) |
| M161 | Startup Compositor Crash | done | - | bosley-native-nodes | E10 | | | [M161-startup-compositor-crash.md](PLAN/MILESTONES/M161-startup-compositor-crash.md) |
| M170 | Engine Render Races | done | - | bosley-native-nodes | E14 | | | [M170-engine-render-races.md](PLAN/MILESTONES/M170-engine-render-races.md) |
| M162 | OFX CUDA And OpenCL Suites | todo | M86 |  | E5 | | | [M162-ofx-cuda-and-opencl-suites.md](PLAN/MILESTONES/M162-ofx-cuda-and-opencl-suites.md) |
| M171 | GPU Correctness Fixes | done | - | gpu | E5 | | | [M171-gpu-correctness-fixes.md](PLAN/MILESTONES/M171-gpu-correctness-fixes.md) |
| M163 | Incremental Bezier Edits | done | - | bosley-native-nodes | E6 | | | [M163-incremental-bezier-edits.md](PLAN/MILESTONES/M163-incremental-bezier-edits.md) |
| M164 | Viewer Node Knobs | done | M16, M107 | bosley-native-nodes | E3 | | | [M164-viewer-node-knobs.md](PLAN/MILESTONES/M164-viewer-node-knobs.md) |
| M165 | Rich Viewer Overlays | done | M80 | misty-marten | E3 | | | [M165-rich-viewer-overlays.md](PLAN/MILESTONES/M165-rich-viewer-overlays.md) |
| M166 | Non-Float Plane Model | todo | - |  | E14 | | | [M166-non-float-plane-model.md](PLAN/MILESTONES/M166-non-float-plane-model.md) |
| M172 | GL Widget Context Loss | done | - | bosley-native-nodes | E10 | | | [M172-gl-widget-context-loss.md](PLAN/MILESTONES/M172-gl-widget-context-loss.md) |
| M167 | Per-Frame Node Metadata | done | M100 |  | E14 | | | [M167-per-frame-node-metadata.md](PLAN/MILESTONES/M167-per-frame-node-metadata.md) |
| M168 | NVIDIA Viewer Hand-off Validation | done | M107, M91 | bosley-faux | E5 | | | [M168-nvidia-viewer-hand-off-validation.md](PLAN/MILESTONES/M168-nvidia-viewer-hand-off-validation.md) |
| M169 | Pipelined Strip Member Timing | done | M107, M91 | bosley-faux | E5 | | | [M169-pipelined-strip-member-timing.md](PLAN/MILESTONES/M169-pipelined-strip-member-timing.md) |
| M173 | Global Mode For Value Knobs | done | M119 | bosley-native-nodes | E10 | |  | [M173-global-mode-for-value-knobs.md](PLAN/MILESTONES/M173-global-mode-for-value-knobs.md) |
| M174 | Wayland Support | blocked | - |  | E1 | | | [M174-wayland-support.md](PLAN/MILESTONES/M174-wayland-support.md) |
| M175 | Build Script Jobs And Test Legs | done | - | bosley-native-nodes | E2 | | | [M175-build-script-jobs-and-test-legs.md](PLAN/MILESTONES/M175-build-script-jobs-and-test-legs.md) |
| M176 | Splice-Vertical Regression | done | - |  | E10 | | | [M176-splice-vertical-regression.md](PLAN/MILESTONES/M176-splice-vertical-regression.md) |
| M178 | Render Context Core | done | M167 |  | E13 | | | [M178-render-context-core.md](PLAN/MILESTONES/M178-render-context-core.md) |
| M179 | Render Graph Continuation | done | M178 | | E6 | | | [M179-render-graph-continuation.md](PLAN/MILESTONES/M179-render-graph-continuation.md) |
| M180 | Loop Block Core | done | M178 | bosley-native-nodes | E15 | | | [M180-loop-block-core.md](PLAN/MILESTONES/M180-loop-block-core.md) |
| M181 | Loop Drivers And UI | done | M180, M71 | bosley-native-nodes | E15 | | | [M181-loop-drivers-and-ui.md](PLAN/MILESTONES/M181-loop-drivers-and-ui.md) |
| M182 | Loop Feedback | done | M180 | bosley-native-nodes | E15 | | | [M182-loop-feedback.md](PLAN/MILESTONES/M182-loop-feedback.md) |
| M183 | Loop Map Blend | done | M180, M179, M184 | | E15 | F14 | | [M183-loop-map-blend.md](PLAN/MILESTONES/M183-loop-map-blend.md) |
| M184 | Variable-Size Filter Kit | done | M120, M137 | gpu | E15 | | | [M184-variable-size-filter-kit.md](PLAN/MILESTONES/M184-variable-size-filter-kit.md) |
| M185 | Variable Warps | done | M113, M184 |  | E15 | | | [M185-variable-warps.md](PLAN/MILESTONES/M185-variable-warps.md) |
| M186 | Exact IDefocus | done | M131, M184 | bosley-native-nodes | E15 | | | [M186-exact-idefocus.md](PLAN/MILESTONES/M186-exact-idefocus.md) |
| M187 | Variable Sharpen And Glow | blocked | M123, M184 |  | E15 | | | [M187-variable-sharpen-and-glow.md](PLAN/MILESTONES/M187-variable-sharpen-and-glow.md) |
| M188 | Variable Median And DirBlur | blocked | M130, M184 |  | E15 | | | [M188-variable-median-and-dirblur.md](PLAN/MILESTONES/M188-variable-median-and-dirblur.md) |
| M189 | Make I-Node Action | done | M183, M184, M181 | | E15 | F14 | | [M189-make-i-node-action.md](PLAN/MILESTONES/M189-make-i-node-action.md) |
| M190 | Cross-Frame Feedback | done | M182 | bosley-native-nodes | E15 | | | [M190-cross-frame-feedback.md](PLAN/MILESTONES/M190-cross-frame-feedback.md) |
| M191 | Native VectorBlur | done | M120, M137 | bosley-native-nodes | E3 | | | [M191-native-vectorblur.md](PLAN/MILESTONES/M191-native-vectorblur.md) |
| M177 | Renders Isolated From Edits | done | M167 |  | E14 | | | [M177-renders-isolated-from-edits.md](PLAN/MILESTONES/M177-renders-isolated-from-edits.md) |
| M192 | Reliable Pipe Pick-Up | done | - |  | E10 | | | [M192-reliable-pipe-pick-up.md](PLAN/MILESTONES/M192-reliable-pipe-pick-up.md) |
| M193 | Pipe Hint Highlight Colour | done | M176 |  | E10 | | | [M193-pipe-hint-highlight-colour.md](PLAN/MILESTONES/M193-pipe-hint-highlight-colour.md) |
| M194 | Kiss Connections | done | M192, M193 | bosley-native-nodes | E10 | | | [M194-kiss-connections.md](PLAN/MILESTONES/M194-kiss-connections.md) |
| M195 | Shake And Cut Disconnect | done | M194 | bosley-native-nodes | E10 | | | [M195-shake-and-cut-disconnect.md](PLAN/MILESTONES/M195-shake-and-cut-disconnect.md) |
| M196 | Copy-Compute Contention | done | M156 | gpu | E5 | | | [M196-copy-compute-contention.md](PLAN/MILESTONES/M196-copy-compute-contention.md) |
| M197 | Python Undo Step API | done | M114 | bosley-native-nodes | E7 | | | [M197-python-undo-step-api.md](PLAN/MILESTONES/M197-python-undo-step-api.md) |
| M198 | RenderEngine On Node | todo | M177 |  | E14 | | | [M198-renderengine-on-node.md](PLAN/MILESTONES/M198-renderengine-on-node.md) |
| M199 | Remove Legacy Scheduler | doing | - | bosley-executor | E14 | | | [M199-remove-legacy-scheduler.md](PLAN/MILESTONES/M199-remove-legacy-scheduler.md) |
| M200 | Folded Knob Shared Storage | todo | - |  | E14 | | | [M200-folded-knob-shared-storage.md](PLAN/MILESTONES/M200-folded-knob-shared-storage.md) |
| M201 | Trimap Removal | todo | M199 |  | E14 | | | [M201-trimap-removal.md](PLAN/MILESTONES/M201-trimap-removal.md) |
| M202 | Clean-Hash Cache Stores | done | M167 | bosley-native-nodes | E14 | | | [M202-clean-hash-cache-stores.md](PLAN/MILESTONES/M202-clean-hash-cache-stores.md) |
| M203 | ONNX Inference Backend | done | - |  | E16 | | | [M203-onnx-inference-backend.md](PLAN/MILESTONES/M203-onnx-inference-backend.md) |
| M204 | AI Model Manager | done | M203 | bosley-native-nodes | E16 | | | [M204-ai-model-manager.md](PLAN/MILESTONES/M204-ai-model-manager.md) |
| M205 | Object Mask To Roto | done | M203, M204, M146 | bosley-native-nodes | E16 | | | [M205-object-mask-to-roto.md](PLAN/MILESTONES/M205-object-mask-to-roto.md) |
| M206 | Neural Raw Denoise | done | M203, M204 | misty-marten | E16 | | | [M206-neural-raw-denoise.md](PLAN/MILESTONES/M206-neural-raw-denoise.md) |
| M207 | Neural Denoise | done | M203, M204 | bosley-native-nodes | E16 | | | [M207-neural-denoise.md](PLAN/MILESTONES/M207-neural-denoise.md) |
| M208 | Neural Upscale | done | M203, M204, M207 | bosley-native-nodes | E16 | | | [M208-neural-upscale.md](PLAN/MILESTONES/M208-neural-upscale.md) |
| M209 | Edge-Aware Filter Library | done | - | bosley-native-nodes | E17 | | | [M209-edge-aware-filter-library.md](PLAN/MILESTONES/M209-edge-aware-filter-library.md) |
| M210 | Profiled Denoise | done | M209 |  | E17 | | | [M210-profiled-denoise.md](PLAN/MILESTONES/M210-profiled-denoise.md) |
| M211 | Wavelet Retouch | done | M209, M146 | bosley-native-nodes | E17 | | | [M211-wavelet-retouch.md](PLAN/MILESTONES/M211-wavelet-retouch.md) |
| M212 | Tone Equalizer | done | M209 | bosley-native-nodes | E17 | | | [M212-tone-equalizer.md](PLAN/MILESTONES/M212-tone-equalizer.md) |
| M213 | Contrast And Texture | done | M209 |  | E17 | | | [M213-contrast-and-texture.md](PLAN/MILESTONES/M213-contrast-and-texture.md) |
| M214 | Colour Balance RGB | done | - | bosley-native-nodes | E17 | | | [M214-colour-balance-rgb.md](PLAN/MILESTONES/M214-colour-balance-rgb.md) |
| M215 | Colour Equalizer | done | M209, M214 | bosley-native-nodes | E17 | | | [M215-colour-equalizer.md](PLAN/MILESTONES/M215-colour-equalizer.md) |
| M216 | Auto-Straighten | done | - | bosley-native-nodes | E17 | | | [M216-auto-straighten.md](PLAN/MILESTONES/M216-auto-straighten.md) |
| M217 | VectorGenerator Node | done | M124 | bosley-native-nodes | E18 | F8 | | [M217-vectorgenerator-node.md](PLAN/MILESTONES/M217-vectorgenerator-node.md) |
| M218 | VectorGenerator GPU | done | M217, M124 | | E18 | F8 | | [M218-vectorgenerator-gpu.md](PLAN/MILESTONES/M218-vectorgenerator-gpu.md) |
| M219 | Curves Follow Vectors | done | M217, M146 | | E18 | F8 | | [M219-curves-follow-vectors.md](PLAN/MILESTONES/M219-curves-follow-vectors.md) |
| M220 | SplineWarp Follow Vectors | done | M219, M148 | bosley-native-nodes | E18 | F8 | | [M220-splinewarp-follow-vectors.md](PLAN/MILESTONES/M220-splinewarp-follow-vectors.md) |
| M221 | Neural Optical Flow | done | M217, M203, M204 | bosley-native-nodes | E16 | F8 | | [M221-neural-optical-flow.md](PLAN/MILESTONES/M221-neural-optical-flow.md) |
| M222 | User Control Of GPU Placement | todo | M132 |  | E5 | | | [M222-user-control-of-gpu-placement.md](PLAN/MILESTONES/M222-user-control-of-gpu-placement.md) |
| M223 | Read And Write Knob Cleanup | done | M167 | bosley-native-nodes | E3 | | | [M223-read-and-write-knob-cleanup.md](PLAN/MILESTONES/M223-read-and-write-knob-cleanup.md) |
| M224 | Fluid Roto And RotoPaint Interaction | done | M146, M117 | misty-marten | E6 | | | [M224-fluid-roto-and-rotopaint-interaction.md](PLAN/MILESTONES/M224-fluid-roto-and-rotopaint-interaction.md) |
| M225 | ~~Clean Shutdown And Memory Release~~ | cancelled | - |  | E14 | | | [M225-clean-shutdown-and-memory-release.md](PLAN/MILESTONES/M225-clean-shutdown-and-memory-release.md) |
| M260 | Cache Invalidation Follow-ups | done | M242 | bosley-native-nodes | E14 | | | [M260-cache-invalidation-follow-ups.md](PLAN/MILESTONES/M260-cache-invalidation-follow-ups.md) |
| M261 | Roto Keyframes In Dope Sheet And Curve Editor | done | M163 | bosley-native-nodes | E6 | | | [M261-roto-keyframes-in-dope-sheet-and-curve-editor.md](PLAN/MILESTONES/M261-roto-keyframes-in-dope-sheet-and-curve-editor.md) |
| M268 | Edge-Aware Region Independence | done | M209, M210, M211, M212, M213, M215 | bosley-native-nodes | E17 | |  | [M268-edge-aware-region-independence.md](PLAN/MILESTONES/M268-edge-aware-region-independence.md) |
| M269 | Remote GUI Checks | done |  | bosley-native-nodes | E2 | | | [M269-remote-gui-checks.md](PLAN/MILESTONES/M269-remote-gui-checks.md) |
| M262 | Expression Benchmark For Large Graphs | done | - | | E14 | F12 | | [M262-expression-benchmark-large-graphs.md](PLAN/MILESTONES/M262-expression-benchmark-large-graphs.md) |
| M263 | Expression Interpreter Overhead | todo | M262, M266 |  | E14 | F12 | | [M263-expression-interpreter-overhead.md](PLAN/MILESTONES/M263-expression-interpreter-overhead.md) |
| M264 | Expression Result Cache | todo | M263 |  | E14 | F12 | | [M264-expression-result-cache.md](PLAN/MILESTONES/M264-expression-result-cache.md) |
| M265 | Native Expression Evaluator | todo | M264 |  | E14 | F12 | | [M265-native-expression-evaluator.md](PLAN/MILESTONES/M265-native-expression-evaluator.md) |
| M266 | Expression Namespace Spike | todo | M262 |  | E14 | F12 | | [M266-expression-namespace-spike.md](PLAN/MILESTONES/M266-expression-namespace-spike.md) |
| M267 | RFVSwitch Match Choice Button | done | M72 | bosley-native-nodes | E13 | | | [M267-rfvswitch-match-choice-button.md](PLAN/MILESTONES/M267-rfvswitch-match-choice-button.md) |
| M270 | Lanczos Sinc Resampling Filters | done | - | misty-marten | E3 | | | [M270-lanczos-sinc-resampling-filters.md](PLAN/MILESTONES/M270-lanczos-sinc-resampling-filters.md) |
| M271 | Persistent Message Test Failures | done | - | bosley-native-nodes | E14 | | | [M271-persistent-message-test-failures.md](PLAN/MILESTONES/M271-persistent-message-test-failures.md) |
| M272 | Script Writes To Selected Roto Shapes | done | - | bosley-native-nodes | E14 | | | [M272-script-writes-to-selected-roto-shapes.md](PLAN/MILESTONES/M272-script-writes-to-selected-roto-shapes.md) |
| M273 | RenderIsolation Exit Hang | done | - | bosley-native-nodes | E6 | | | [M273-renderisolation-exit-hang.md](PLAN/MILESTONES/M273-renderisolation-exit-hang.md) |
| M274 | SplineWarp Remaining Controls | done | M147, M148, M149, M150, M151 | bosley-native-nodes | E3 | | | [M274-splinewarp-remaining-controls.md](PLAN/MILESTONES/M274-splinewarp-remaining-controls.md) |
| M275 | Viewer Widget Editing | done | - | bosley-native-nodes | E10 | | | [M275-viewer-widget-editing.md](PLAN/MILESTONES/M275-viewer-widget-editing.md) |
| M276 | Upstream Reuse On Edits | done | M260 | misty-marten | E6 | F6 | | [M276-cache-responsiveness-audit.md](PLAN/MILESTONES/M276-cache-responsiveness-audit.md) |
| M300 | Unified Tiered Cache | done | M276 | bosley-native-nodes | E6 | F6 | | [M300-unified-tiered-cache.md](PLAN/MILESTONES/M300-unified-tiered-cache.md) |
| M301 | GPU Cache Tier | done | M300 | bosley-native-nodes | E6 | F6 | | [M301-gpu-cache-tier.md](PLAN/MILESTONES/M301-gpu-cache-tier.md) |
| M302 | Viewer Render State Cues | done | - | bosley-native-nodes | E6 | F6 | | [M302-viewer-render-state-cues.md](PLAN/MILESTONES/M302-viewer-render-state-cues.md) |
| M303 | Interactive Edit Latency | done | M301 | bosley-native-nodes | E6 | F6 | | [M303-interactive-edit-latency.md](PLAN/MILESTONES/M303-interactive-edit-latency.md) |
| M277 | Graph Creation And Insert | done | M192, M193 | bosley-native-nodes | E10 | | | [M277-graph-creation-and-insert.md](PLAN/MILESTONES/M277-graph-creation-and-insert.md) |
| M278 | Translucent Selection Highlight | done | M234 | | E9 | | | [M278-translucent-selection-highlight.md](PLAN/MILESTONES/M278-translucent-selection-highlight.md) |
| M279 | Utility Renderer Foundation | done | M248 | bosley-native-nodes | E19 | F7 | | [M279-utility-renderer-foundation.md](PLAN/MILESTONES/M279-utility-renderer-foundation.md) |
| M280 | Utility Renderer GPU Path | done | M279, M253 | bosley-native-nodes | E19 | F7 | | [M280-utility-renderer-gpu-path.md](PLAN/MILESTONES/M280-utility-renderer-gpu-path.md) |
| M281 | Utility Materials And Lights | done | M280, M233, M234 | bosley-native-nodes | E19 | F7 | | [M281-utility-materials-and-lights.md](PLAN/MILESTONES/M281-utility-materials-and-lights.md) |
| M283 | Utility Sampling And Motion Blur | done | M280, M253 | bosley-native-nodes | E19 | F7 | | [M283-utility-sampling-and-motion-blur.md](PLAN/MILESTONES/M283-utility-sampling-and-motion-blur.md) |
| M282 | Utility Reflection And Refraction | done | M281, M283 | | E19 | F7 | | [M282-utility-reflection-and-refraction.md](PLAN/MILESTONES/M282-utility-reflection-and-refraction.md) |
| M284 | Utility Renderer Subdivision | done | M280 |  | E19 | F7 | | [M284-utility-renderer-subdivision.md](PLAN/MILESTONES/M284-utility-renderer-subdivision.md) |
| M285 | Utility Renderer AOVs | todo | M256, M282, M283 |  | E19 | | | [M285-utility-renderer-aovs.md](PLAN/MILESTONES/M285-utility-renderer-aovs.md) |
| M286 | Utility Renderer Deep Output | todo | M285 |  | E19 | F24 | | [M286-utility-renderer-deep-output.md](PLAN/MILESTONES/M286-utility-renderer-deep-output.md) |
| M288 | Utility Renderer Projection Modes | done | M281, M283 | | E19 | F7 | | [M288-utility-renderer-projection-modes.md](PLAN/MILESTONES/M288-utility-renderer-projection-modes.md) |
| M290 | Native Cryptomatte Keyer | todo | M285 |  | E19 | | | [M290-native-cryptomatte-keyer.md](PLAN/MILESTONES/M290-native-cryptomatte-keyer.md) |
| M289 | Utility Adaptive Subdivision | todo | M284 |  | E19 | | | [M289-utility-adaptive-subdivision.md](PLAN/MILESTONES/M289-utility-adaptive-subdivision.md) |
| M287 | Utility Renderer Displacement | todo | M289 |  | E19 | | | [M287-utility-renderer-displacement.md](PLAN/MILESTONES/M287-utility-renderer-displacement.md) |
| M291 | Defocus Reference Harness | done | M131, M133 | | E24 | F13 | | [M291-defocus-reference-harness.md](PLAN/MILESTONES/M291-defocus-reference-harness.md) |
| M292 | Traced Lens Bokeh | todo | M131, M133, M462, M463, M465 |  | E24 | F13 | | [M292-traced-lens-bokeh.md](PLAN/MILESTONES/M292-traced-lens-bokeh.md) |
| M293 | ZDefocus Debug Outputs | done | M131, M133 | | E3 | F13 | | [M293-zdefocus-debug-outputs.md](PLAN/MILESTONES/M293-zdefocus-debug-outputs.md) |
| M473 | Lens Camera And Focus Controls | todo | M292 |  | E24 | F13 | | [M473-lens-camera-and-focus-controls.md](PLAN/MILESTONES/M473-lens-camera-and-focus-controls.md) |
| M474 | Traced Lens Creative Aberrations | todo | M292 |  | E24 | F13 | | [M474-traced-lens-creative-aberrations.md](PLAN/MILESTONES/M474-traced-lens-creative-aberrations.md) |
| M475 | Aperture Maps And Front Filter | todo | M292, M474 |  | E24 | F13 | | [M475-aperture-maps-and-front-filter.md](PLAN/MILESTONES/M475-aperture-maps-and-front-filter.md) |
| M476 | Traced Anamorphic Bokeh | todo | M292 |  | E24 | F13 | | [M476-traced-anamorphic-bokeh.md](PLAN/MILESTONES/M476-traced-anamorphic-bokeh.md) |
| M477 | Lens Cross-Section Panel | todo | M292, M473, M474, M476 |  | E24 | F13 | | [M477-lens-cross-section-panel.md](PLAN/MILESTONES/M477-lens-cross-section-panel.md) |
| M478 | Cinema And Vintage Lens Prescriptions | todo | M462, M463 |  | E24 | F13 | | [M478-cinema-and-vintage-lens-prescriptions.md](PLAN/MILESTONES/M478-cinema-and-vintage-lens-prescriptions.md) |
| M294 | ZDefocus Engine Revisit | todo | M291, M292, M293, M473, M474, M475, M476 |  | E24 | F13 | | [M294-zdefocus-engine-revisit.md](PLAN/MILESTONES/M294-zdefocus-engine-revisit.md) |
| M479 | Lens Bokeh Parity Audit | todo | M292, M294, M473, M474, M475, M476, M477, M478 |  | E24 | F13 | | [M479-lens-bokeh-parity-audit.md](PLAN/MILESTONES/M479-lens-bokeh-parity-audit.md) |
| M295 | Native DeepDefocus | todo | M294 |  | E8 | | | [M295-native-deepdefocus.md](PLAN/MILESTONES/M295-native-deepdefocus.md) |
| M462 | Shared Optics Library | done | - | | E24 | F30 | | [M462-shared-optics-library.md](PLAN/MILESTONES/M462-shared-optics-library.md) |
| M463 | Bundled Lens Database | done | M462 | | E24 | F30 | | [M463-bundled-lens-database.md](PLAN/MILESTONES/M463-bundled-lens-database.md) |
| M465 | Aperture Model | done | - | | E24 | F30 | | [M465-aperture-model.md](PLAN/MILESTONES/M465-aperture-model.md) |
| M464 | FFT Library | done | - | | E24 | F31 | | [M464-fft-library.md](PLAN/MILESTONES/M464-fft-library.md) |
| M472 | GPU Raster Pipelines | done | - | | E5 |  | | [M472-gpu-raster-pipelines.md](PLAN/MILESTONES/M472-gpu-raster-pipelines.md) |
| M466 | LensFlare Ghost Tracing | todo | M462, M463, M464, M465 |  | E24 | F31 | | [M466-lensflare-ghost-tracing.md](PLAN/MILESTONES/M466-lensflare-ghost-tracing.md) |
| M467 | LensFlare GPU Ghosts | todo | M466, M472 |  | E24 | F31 | | [M467-lensflare-gpu-ghosts.md](PLAN/MILESTONES/M467-lensflare-gpu-ghosts.md) |
| M468 | LensFlare Starburst | todo | M464, M465, M466 |  | E24 | F31 | | [M468-lensflare-starburst.md](PLAN/MILESTONES/M468-lensflare-starburst.md) |
| M469 | LensFlare Layers And Diagram | todo | M466, M468 |  | E24 | F31 | | [M469-lensflare-layers-and-diagram.md](PLAN/MILESTONES/M469-lensflare-layers-and-diagram.md) |
| M470 | LensFlare From USD Lights | todo | M466, M468 |  | E24 | F31 | | [M470-lensflare-from-usd-lights.md](PLAN/MILESTONES/M470-lensflare-from-usd-lights.md) |
| M471 | Lens Browser Previews | todo | M463, M466, M467, M468 |  | E24 | F31 | | [M471-lens-browser-previews.md](PLAN/MILESTONES/M471-lens-browser-previews.md) |
| M480 | LensFlare Creative Aberrations | todo | M474, M466, M467, M468 |  | E24 |  | | [M480-lensflare-creative-aberrations.md](PLAN/MILESTONES/M480-lensflare-creative-aberrations.md) |
| M296 | Raw Prepare Output Format | done | M80 | misty-marten | E3 | | | [M296-raw-prepare-output-format.md](PLAN/MILESTONES/M296-raw-prepare-output-format.md) |
| M297 | Viewer Node Knobs Follow-ups | done | M164 | | E3 | | | [M297-viewer-node-knobs-followups.md](PLAN/MILESTONES/M297-viewer-node-knobs-followups.md) |
| M298 | RenderIsolation Taskgraph Hang | done | M177 | bosley-native-nodes | E3 | | | [M298-renderisolation-taskgraph-hang.md](PLAN/MILESTONES/M298-renderisolation-taskgraph-hang.md) |
| M299 | Roto Render Performance | doing | M224, M276, M300 | bosley-executor | E6 | | | [M299-roto-render-performance.md](PLAN/MILESTONES/M299-roto-render-performance.md) |
| M304 | Roto Points Snapping To Origin | done | M224, M261 | bosley-native-nodes | E6 | | | [M304-roto-points-snapping-to-origin.md](PLAN/MILESTONES/M304-roto-points-snapping-to-origin.md) |
| M305 | Stale Viewer Keyframe Markers | done | M275, M261 | misty-marten | E10 | F10 | | [M305-stale-viewer-keyframe-markers.md](PLAN/MILESTONES/M305-stale-viewer-keyframe-markers.md) |
| M306 | ~~Knob Row Layout~~ | cancelled | M229 | misty-marten | E10 | F10 | | [M306-knob-row-layout.md](PLAN/MILESTONES/M306-knob-row-layout.md) |
| M307 | Remove Viewer Auto-Contrast | done | M164 | misty-marten | E10 | F10 | | [M307-remove-viewer-auto-contrast.md](PLAN/MILESTONES/M307-remove-viewer-auto-contrast.md) |
| M308 | Knob Label Cleanup | todo | - |  | E10 | | | [M308-knob-label-cleanup.md](PLAN/MILESTONES/M308-knob-label-cleanup.md) |
| M309 | Roto Feather Falloff Banding | done | M224 | | E6 | | | [M309-roto-feather-falloff-banding.md](PLAN/MILESTONES/M309-roto-feather-falloff-banding.md) |
| M310 | Device-Switch Shader Test Fix | done | - | bosley-native-nodes | E14 | | | [M310-device-switch-shader-test-fix.md](PLAN/MILESTONES/M310-device-switch-shader-test-fix.md) |
| M311 | UDIM Live Textures | done | M233 | bosley-native-nodes | E9 | F11 | | [M311-udim-live-textures.md](PLAN/MILESTONES/M311-udim-live-textures.md) |
| M312 | USDLayerMaterial Node | todo | M53 |  | E9 | F11 | | [M312-usdlayermaterial-node.md](PLAN/MILESTONES/M312-usdlayermaterial-node.md) |
| M313 | USDBakeTextures Node | todo | M53, M234 |  | E9 | F11 | | [M313-usdbaketextures-node.md](PLAN/MILESTONES/M313-usdbaketextures-node.md) |
| M314 | Paint-Through Inputs | todo | M53, M234 |  | E9 | F11 | | [M314-paint-through-inputs.md](PLAN/MILESTONES/M314-paint-through-inputs.md) |
| M315 | Direct Texel Painting | todo | M53 |  | E9 | F11 | | [M315-direct-texel-painting.md](PLAN/MILESTONES/M315-direct-texel-painting.md) |
| M316 | Delegate UV Baking | todo | M313, M251 |  | E9 | | | [M316-delegate-uv-baking.md](PLAN/MILESTONES/M316-delegate-uv-baking.md) |
| M427 | Native Geometry Texture Painting | todo | M53, M358, M376, M428 |  | E9 | F11 | | [M427-native-geometry-texture-painting.md](PLAN/MILESTONES/M427-native-geometry-texture-painting.md) |
| M428 | Native UV Operators | todo | M359 |  | E9 | F11 | | [M428-native-uv-operators.md](PLAN/MILESTONES/M428-native-uv-operators.md) |
| M429 | GeoAttribPaint Node | todo | M53, M359, M376 |  | E9 | F11 | | [M429-geoattribpaint-node.md](PLAN/MILESTONES/M429-geoattribpaint-node.md) |
| M430 | Viewer 2D/3D Switch | todo | M53, M376 |  | E9 | F11 | | [M430-viewer-2d-3d-switch.md](PLAN/MILESTONES/M430-viewer-2d-3d-switch.md) |
| M431 | Fast Panel Build | doing | - | bosley-executor | E10 |  | uat | [M431-fast-panel-build.md](PLAN/MILESTONES/M431-fast-panel-build.md) |
| M317 | Traditional Keyer | todo | M95 |  | E3 | | | [M317-traditional-keyer.md](PLAN/MILESTONES/M317-traditional-keyer.md) |
| M318 | LensCorrect Bounded Input Region | done | M160 | bosley-native-nodes | E3 | | | [M318-lenscorrect-bounded-input-region.md](PLAN/MILESTONES/M318-lenscorrect-bounded-input-region.md) |
| M319 | Group Node Direct Render | done | - | bosley-native-nodes | E14 | | | [M319-group-node-direct-render.md](PLAN/MILESTONES/M319-group-node-direct-render.md) |
| M320 | Write All Input Metadata | done | - | misty-marten | E3 | | | [M320-write-all-input-metadata.md](PLAN/MILESTONES/M320-write-all-input-metadata.md) |
| M321 | Render Thread Count Race | done | - | bosley-native-nodes | E2 | | | [M321-render-thread-count-race.md](PLAN/MILESTONES/M321-render-thread-count-race.md) |
| M322 | NVIDIA Exit Deadlock Fix | done | - | bosley-native-nodes | E14 | |  | [M322-nvidia-exit-deadlock-fix.md](PLAN/MILESTONES/M322-nvidia-exit-deadlock-fix.md) |
| M323 | SplineWarp Map Input | todo | M274 |  | E3 | | | [M323-splinewarp-map-input.md](PLAN/MILESTONES/M323-splinewarp-map-input.md) |
| M324 | SplineWarp STMap And IDistort Outputs | todo | M274 |  | E3 | | | [M324-splinewarp-stmap-idistort-outputs.md](PLAN/MILESTONES/M324-splinewarp-stmap-idistort-outputs.md) |
| M325 | MoonRay Fork | done | M252 | bosley-native-nodes | E9 | | | [M325-moonray-fork.md](PLAN/MILESTONES/M325-moonray-fork.md) |
| M326 | Legible Viewer Warning Banner | done | - | misty-marten | E10 | F10 | | [M326-legible-viewer-warning-banner.md](PLAN/MILESTONES/M326-legible-viewer-warning-banner.md) |
| M338 | Viewer Input Keys And Locks | done | - | misty-marten | E10 | F10 | | [M338-viewer-input-keys-and-locks.md](PLAN/MILESTONES/M338-viewer-input-keys-and-locks.md) |
| M339 | File Knob Reload Button Cleanup | done | M52 | misty-marten | E10 | F10 | | [M339-file-knob-reload-button-cleanup.md](PLAN/MILESTONES/M339-file-knob-reload-button-cleanup.md) |
| M340 | Viewer Gain Gamma On Rendered Pixels | done | M234 | bosley-native-nodes | E9 | | | [M340-viewer-gain-gamma-on-rendered-pixels.md](PLAN/MILESTONES/M340-viewer-gain-gamma-on-rendered-pixels.md) |
| M341 | Per-Placement Geometry Projection | done | M234 | bosley-native-nodes | E9 | F9 | | [M341-per-placement-geometry-projection.md](PLAN/MILESTONES/M341-per-placement-geometry-projection.md) |
| M342 | Sticky Projections On Deforming Geometry | done | M234 | bosley-native-nodes | E9 | F9 | | [M342-sticky-projections-deforming-geometry.md](PLAN/MILESTONES/M342-sticky-projections-deforming-geometry.md) |
| M343 | Live Textures In Delegates | done | M233, M251, M252, M254 | | E9 | F9 | | [M343-live-textures-in-delegates.md](PLAN/MILESTONES/M343-live-textures-in-delegates.md) |
| M344 | Camera Projection In Delegates | done | M234, M325, M343 | bosley-native-nodes | E9 | F9 | | [M344-camera-projection-in-delegates.md](PLAN/MILESTONES/M344-camera-projection-in-delegates.md) |
| M345 | VDB Rendering Fixes | doing | M251, M252, M325 | bosley-executor | E9 | F1 | | [M345-vdb-rendering-fixes.md](PLAN/MILESTONES/M345-vdb-rendering-fixes.md) |
| M346 | USDVolume Node | todo | M227, M345 |  | E9 | F1 | | [M346-usdvolume-node.md](PLAN/MILESTONES/M346-usdvolume-node.md) |
| M347 | Volume Material Node | todo | M236, M345, M346 |  | E9 | F1 | | [M347-volume-material-node.md](PLAN/MILESTONES/M347-volume-material-node.md) |
| M348 | Volume Viewer Support | todo | M229, M231, M232, M253, M333, M345, M346 |  | E9 | F1 | | [M348-volume-viewer-support.md](PLAN/MILESTONES/M348-volume-viewer-support.md) |
| M349 | Shader Node Spike | done | - | | E9 | F2 | | [M349-shader-node-spike.md](PLAN/MILESTONES/M349-shader-node-spike.md) |
| M350 | Shader Manifest And Nodes | doing | M349, M253 | bosley-executor | E9 | F2 | | [M350-shader-manifest-and-nodes.md](PLAN/MILESTONES/M350-shader-manifest-and-nodes.md) |
| M351 | Shader Graph Presentation | todo | M350 |  | E9 | F2 | | [M351-shader-graph-presentation.md](PLAN/MILESTONES/M351-shader-graph-presentation.md) |
| M352 | USDMaterialBuilder Node | todo | M350, M351 |  | E9 | F2 | | [M352-usdmaterialbuilder-node.md](PLAN/MILESTONES/M352-usdmaterialbuilder-node.md) |
| M353 | Cycles Native Shaders | todo | M352, M343 |  | E9 | F2 | | [M353-cycles-native-shaders.md](PLAN/MILESTONES/M353-cycles-native-shaders.md) |
| M354 | MoonRay Native Shaders | todo | M352, M325, M343 |  | E9 | F2 | | [M354-moonray-native-shaders.md](PLAN/MILESTONES/M354-moonray-native-shaders.md) |
| M355 | Shader Tweak Node | todo | M343, M353, M354 |  | E9 | F2 | | [M355-shader-tweak-node.md](PLAN/MILESTONES/M355-shader-tweak-node.md) |
| M356 | Automatic Preview Shader | todo | M352 |  | E9 | F2 | | [M356-automatic-preview-shader.md](PLAN/MILESTONES/M356-automatic-preview-shader.md) |
| M357 | Repo Cleanup | todo | - |  | E14 |  | | [M357-repo-cleanup.md](PLAN/MILESTONES/M357-repo-cleanup.md) |
| M364 | Delegate Cancel Responsiveness | done | M254 | bosley-native-nodes | E9 | F9 | | [M364-delegate-cancel-responsiveness.md](PLAN/MILESTONES/M364-delegate-cancel-responsiveness.md) |
| M365 | Delegate Display Transform | doing | M254, M340 | bosley-executor | E9 |  | response | [M365-delegate-display-transform.md](PLAN/MILESTONES/M365-delegate-display-transform.md) |
| M368 | Native Instance Material Binding | done | M341, M342 | bosley-native-nodes | E9 | F9 | | [M368-native-instance-material-binding.md](PLAN/MILESTONES/M368-native-instance-material-binding.md) |
| M366 | Mixed Pixel Aspect Tiles | todo | M300 |  | E14 |  | | [M366-mixed-pixel-aspect-tiles.md](PLAN/MILESTONES/M366-mixed-pixel-aspect-tiles.md) |
| M367 | USD Camera Exposure | todo | M251, M252, M254 |  | E9 |  | | [M367-usd-camera-exposure.md](PLAN/MILESTONES/M367-usd-camera-exposure.md) |
| M369 | Curve Slopes Independent Of Edit Order | todo | M304 |  | E14 |  | | [M369-curve-slopes-independent-of-edit-order.md](PLAN/MILESTONES/M369-curve-slopes-independent-of-edit-order.md) |
| M370 | Roto Point List Render Isolation | done | M304 | | E6 |  | | [M370-roto-point-list-render-isolation.md](PLAN/MILESTONES/M370-roto-point-list-render-isolation.md) |
| M371 | Cycles Abort Test Thread Count | done | - | bosley-native-nodes | E9 |  | | [M371-cycles-abort-test-thread-count.md](PLAN/MILESTONES/M371-cycles-abort-test-thread-count.md) |
| M372 | User Knob Engine Edits | done | - | | E10 | F16 | | [M372-user-knob-engine-edits.md](PLAN/MILESTONES/M372-user-knob-engine-edits.md) |
| M373 | In-Panel Knob Editor | done | M372 | | E10 | F16 | | [M373-in-panel-knob-editor.md](PLAN/MILESTONES/M373-in-panel-knob-editor.md) |
| M374 | Remove Node Magnify On Ctrl-Scroll | done | - | | E10 |  | | [M374-remove-node-magnify-on-ctrl-scroll.md](PLAN/MILESTONES/M374-remove-node-magnify-on-ctrl-scroll.md) |
| M375 | No CPU Vulkan Auto-Select | done | - | bosley-native-nodes | E5 |  | | [M375-no-cpu-vulkan-auto-select.md](PLAN/MILESTONES/M375-no-cpu-vulkan-auto-select.md) |
| M400 | Pointer Direction Badges | done | - | | E10 |  | | [M400-pointer-direction-badges.md](PLAN/MILESTONES/M400-pointer-direction-badges.md) |
| M425 | No ONNX Runtime Telemetry | done | - | bosley-native-nodes | E16 |  | | [M425-no-onnx-runtime-telemetry.md](PLAN/MILESTONES/M425-no-onnx-runtime-telemetry.md) |
| M426 | UDIM Textures In Delegates | todo | M311, M343 |  | E9 |  | | [M426-udim-textures-in-delegates.md](PLAN/MILESTONES/M426-udim-textures-in-delegates.md) |
| M432 | Overlays Off The Render Path | done | - | | E23 |  | | [M432-overlays-off-the-render-path.md](PLAN/MILESTONES/M432-overlays-off-the-render-path.md) |
| M433 | Overlay Point Maps | done | - | | E23 | F25 | | [M433-overlay-point-maps.md](PLAN/MILESTONES/M433-overlay-point-maps.md) |
| M434 | Interacts On The Overlay Draw List | doing | - | bosley-executor | E23 | F25 | | [M434-interacts-on-the-overlay-draw-list.md](PLAN/MILESTONES/M434-interacts-on-the-overlay-draw-list.md) |
| M435 | Overlays Deform Through Warps | todo | M432, M433, M434 |  | E23 | F25 | | [M435-overlays-deform-through-warps.md](PLAN/MILESTONES/M435-overlays-deform-through-warps.md) |
| M436 | Overlays On Projected Geometry | todo | M435 |  | E23 | F25 | | [M436-overlays-on-projected-geometry.md](PLAN/MILESTONES/M436-overlays-on-projected-geometry.md) |
| M437 | CardProject25D | todo | M433, M434 |  | E9 | F25 | | [M437-cardproject25d.md](PLAN/MILESTONES/M437-cardproject25d.md) |
| M438 | Lens Distortion In The USD Scene | done | - | | E9 |  | | [M438-lens-distortion-in-the-usd-scene.md](PLAN/MILESTONES/M438-lens-distortion-in-the-usd-scene.md) |
| M439 | MaterialX Translation Spike | todo | - |  | E9 | F26 | | [M439-materialx-translation-spike.md](PLAN/MILESTONES/M439-materialx-translation-spike.md) |
| M440 | MaterialX Translation Core | todo | M439 |  | E9 | F26 | | [M440-materialx-translation-core.md](PLAN/MILESTONES/M440-materialx-translation-core.md) |
| M441 | MoonRay MaterialX Shaders Build | todo | - |  | E9 | F26 | | [M441-moonray-materialx-shaders-build.md](PLAN/MILESTONES/M441-moonray-materialx-shaders-build.md) |
| M442 | MoonRay MaterialX Backend | todo | M440, M441 |  | E9 | F26 | | [M442-moonray-materialx-backend.md](PLAN/MILESTONES/M442-moonray-materialx-backend.md) |
| M443 | Cycles MaterialX Backend | todo | M440 |  | E9 | F26 | | [M443-cycles-materialx-backend.md](PLAN/MILESTONES/M443-cycles-materialx-backend.md) |
| M444 | Cycles MaterialX Procedurals | todo | M443 |  | E9 | F26 | | [M444-cycles-materialx-procedurals.md](PLAN/MILESTONES/M444-cycles-materialx-procedurals.md) |
| M445 | USDOpenPBR Node | todo | M442, M443, M447 |  | E9 | F26 | | [M445-usdopenpbr-node.md](PLAN/MILESTONES/M445-usdopenpbr-node.md) |
| M446 | MaterialX Builder Nodes | todo | M352, M440, M442, M443, M444 |  | E9 |  | | [M446-materialx-builder-nodes.md](PLAN/MILESTONES/M446-materialx-builder-nodes.md) |
| M447 | USDPreviewSurface Rename | done | - | | E9 |  | | [M447-usdpreviewsurface-rename.md](PLAN/MILESTONES/M447-usdpreviewsurface-rename.md) |
| M448 | USD Layer Break | done | - | | E9 | F27 | | [M448-usd-layer-break.md](PLAN/MILESTONES/M448-usd-layer-break.md) |
| M449 | USD Layer Save Paths | done | M448 | | E9 | F27 | | [M449-usd-layer-save-paths.md](PLAN/MILESTONES/M449-usd-layer-save-paths.md) |
| M450 | USDWrite Export Preview | todo | M448, M449 |  | E9 | F27 | | [M450-usdwrite-export-preview.md](PLAN/MILESTONES/M450-usdwrite-export-preview.md) |
| M451 | Viewer Stale Cue Experiments | done | M302 | | E10 | | | [M451-viewer-stale-cue-experiments.md](PLAN/MILESTONES/M451-viewer-stale-cue-experiments.md) |
| M452 | Node Error And Warning Display | done | M397 | | E10 |  F29 | | [M452-node-error-and-warning-display.md](PLAN/MILESTONES/M452-node-error-and-warning-display.md) |
| M453 | Follow Animation Layer | todo | M220, M486, M487, M488, M489 |  | E26 | F32 | | [M453-follow-animation-layer.md](PLAN/MILESTONES/M453-follow-animation-layer.md) |
| M454 | Roto Insert Point Drag | done | - | | E10 |  F29 | | [M454-roto-insert-point-drag.md](PLAN/MILESTONES/M454-roto-insert-point-drag.md) |
| M455 | Keyframe Markers For Visible Pages | done | M421 | | E10 |  F29 | | [M455-keyframe-markers-for-visible-pages.md](PLAN/MILESTONES/M455-keyframe-markers-for-visible-pages.md) |
| M456 | File Dialog Toolbar And Split | done | - | | E10 |  F29 | | [M456-file-dialog-toolbar-and-split.md](PLAN/MILESTONES/M456-file-dialog-toolbar-and-split.md) |
| M457 | ColorWarp Space Spike | done | - | | E3 | F28 | | [M457-colorwarp-space-spike.md](PLAN/MILESTONES/M457-colorwarp-space-spike.md) |
| M458 | ColorWarp Node | done | M457 | | E3 | F28 | | [M458-colorwarp-node.md](PLAN/MILESTONES/M458-colorwarp-node.md) |
| M459 | ColorWarp 3D View | doing | M458 | bosley-executor | E3 | F28 | | [M459-colorwarp-3d-view.md](PLAN/MILESTONES/M459-colorwarp-3d-view.md) |
| M482 | Geometry VOP Node | todo | M351, M360 |  | E25 | | | [M482-geometry-vop-node.md](PLAN/MILESTONES/M482-geometry-vop-node.md) |
| M483 | Particle VOP Node | todo | M482, M406 |  | E25 | | | [M483-particle-vop-node.md](PLAN/MILESTONES/M483-particle-vop-node.md) |
| M484 | Image Kernel Binding And Wrangle | todo | M360, M408 |  | E25 | | | [M484-image-kernel-binding-and-wrangle.md](PLAN/MILESTONES/M484-image-kernel-binding-and-wrangle.md) |
| M485 | Image VOP Node | todo | M482, M484 |  | E25 | | | [M485-image-vop-node.md](PLAN/MILESTONES/M485-image-vop-node.md) |
| M486 | Channel Network Core | todo | M351 |  | E26 | F32 | | [M486-channel-network-core.md](PLAN/MILESTONES/M486-channel-network-core.md) |
| M487 | Channel Export And chop() | todo | M486 |  | E26 | F32 | | [M487-channel-export-and-chop.md](PLAN/MILESTONES/M487-channel-export-and-chop.md) |
| M488 | Channel Operators And Channel VOP | todo | M486, M482 |  | E26 | F32 | | [M488-channel-operators-and-channel-vop.md](PLAN/MILESTONES/M488-channel-operators-and-channel-vop.md) |
| M489 | Channel Sources From Shapes, Tracks And Cameras | todo | M486, M487 |  | E26 | F32 | | [M489-channel-sources-shapes-tracks-cameras.md](PLAN/MILESTONES/M489-channel-sources-shapes-tracks-cameras.md) |
| M490 | Channel Sources From Images And Audio | todo | M486 |  | E26 | F32 | | [M490-channel-sources-images-and-audio.md](PLAN/MILESTONES/M490-channel-sources-images-and-audio.md) |
| M491 | Extra Input Picker | todo | - |  | E10 | | | [M491-extra-input-picker.md](PLAN/MILESTONES/M491-extra-input-picker.md) |
| M492 | Image-Driven Warp Providers | todo | M153, M179 |  | E14 | | | [M492-image-driven-warp-providers.md](PLAN/MILESTONES/M492-image-driven-warp-providers.md) |
| M493 | Shared Ccache Remote Storage | done | - | | E2 | | | [M493-shared-ccache-remote-storage.md](PLAN/MILESTONES/M493-shared-ccache-remote-storage.md) |
| M549 | Faster Gate Packaging | done | - | | E2 | | | [M549-faster-gate-packaging.md](PLAN/MILESTONES/M549-faster-gate-packaging.md) |
| M550 | Classic Graph Shift-Drop Creation | todo | M351 |  | E9 | | | [M550-classic-graph-shift-drop-creation.md](PLAN/MILESTONES/M550-classic-graph-shift-drop-creation.md) |
| M551 | Second Background App Groups | todo | - |  | E2 | | | [M551-second-background-app-groups.md](PLAN/MILESTONES/M551-second-background-app-groups.md) |
| M553 | Storm Project3D Test Flake | done | - | | E2 | | | [M553-storm-project3d-test-flake.md](PLAN/MILESTONES/M553-storm-project3d-test-flake.md) |
| M552 | Shared Remote Run Watcher | done | - | | E2 | | | [M552-shared-remote-run-watcher.md](PLAN/MILESTONES/M552-shared-remote-run-watcher.md) |
| M494 | Delegate Subdivision | done | - | | E9 | | | [M494-delegate-subdivision.md](PLAN/MILESTONES/M494-delegate-subdivision.md) |
| M495 | KeyMix GPU Edge Read Fix | done | - | | E14 | | | [M495-keymix-gpu-edge-read-fix.md](PLAN/MILESTONES/M495-keymix-gpu-edge-read-fix.md) |
| M496 | MoonRay Test Stability | doing | - | bosley-executor | E9 | | | [M496-moonray-test-stability.md](PLAN/MILESTONES/M496-moonray-test-stability.md) |
| M504 | GIL Lock Order Fix | done | M336 | | E14 |  | | [M504-gil-lock-order-fix.md](PLAN/MILESTONES/M504-gil-lock-order-fix.md) |
| M505 | Deleted Knob Listener Dependencies | done | M372 | | E14 | F16 | | [M505-deleted-knob-listener-dependencies.md](PLAN/MILESTONES/M505-deleted-knob-listener-dependencies.md) |
| M506 | Deep Expression Chain Crash | todo | M262 |  | E14 |  | | [M506-deep-expression-chain-crash.md](PLAN/MILESTONES/M506-deep-expression-chain-crash.md) |
| M507 | Lock-Free Knob Holder Lifetime | todo | - |  | E14 |  | | [M507-lock-free-knob-holder-lifetime.md](PLAN/MILESTONES/M507-lock-free-knob-holder-lifetime.md) |
| M508 | Incremental Roto Dope Sheet Rows | todo | - |  | E6 |  | | [M508-incremental-roto-dope-sheet-rows.md](PLAN/MILESTONES/M508-incremental-roto-dope-sheet-rows.md) |
| M509 | Cancel-Aware GPU Fence Wait | done | - | | E5 |  | | [M509-cancel-aware-gpu-fence-wait.md](PLAN/MILESTONES/M509-cancel-aware-gpu-fence-wait.md) |
| M510 | MoonRay Warning Text | done | - | | E9 |  | | [M510-moonray-warning-text.md](PLAN/MILESTONES/M510-moonray-warning-text.md) |
| M511 | Viewer Toolbar Fits 1920x1080 | done | - | | E10 |  | | [M511-viewer-toolbar-fits-1920x1080.md](PLAN/MILESTONES/M511-viewer-toolbar-fits-1920x1080.md) |
| M512 | Python importTemplate Binding | done | M103 | | E12 | F33 | | [M512-python-importtemplate-binding.md](PLAN/MILESTONES/M512-python-importtemplate-binding.md) |
| M513 | MoonRay OIDN Denoiser | todo | - |  | E9 |  | | [M513-moonray-oidn-denoiser.md](PLAN/MILESTONES/M513-moonray-oidn-denoiser.md) |
| M497 | Lens Schema And Camera Authoring | todo | M132 |  | E9 | F34 | | [M497-lens-schema-and-camera-authoring.md](PLAN/MILESTONES/M497-lens-schema-and-camera-authoring.md) |
| M498 | LensDistortion From Scene | todo | M497 |  | E9 | F34 | | [M498-lensdistortion-from-scene.md](PLAN/MILESTONES/M498-lensdistortion-from-scene.md) |
| M499 | Scene Lens In Projection Nodes | todo | M497, M437 |  | E9 | F34 | | [M499-scene-lens-in-projection-nodes.md](PLAN/MILESTONES/M499-scene-lens-in-projection-nodes.md) |
| M500 | USDProject3D Lens Maps | todo | M497 |  | E9 | F34 | | [M500-usdproject3d-lens-maps.md](PLAN/MILESTONES/M500-usdproject3d-lens-maps.md) |
| M501 | Lens Maps In The Scene | todo | M497, M499, M500, M433 |  | E9 | F34 | | [M501-lens-maps-in-the-scene.md](PLAN/MILESTONES/M501-lens-maps-in-the-scene.md) |
| M514 | Utility Raster Mode | todo | M283, M472 |  | E19 |  | | [M514-utility-raster-mode.md](PLAN/MILESTONES/M514-utility-raster-mode.md) |
| M515 | Storm Oversample And Motion Blur | todo | - |  | E9 |  | | [M515-storm-oversample-and-motion-blur.md](PLAN/MILESTONES/M515-storm-oversample-and-motion-blur.md) |
| M516 | Port Groups And Folding | todo | M351 |  | E9 | F2 | | [M516-port-groups-and-folding.md](PLAN/MILESTONES/M516-port-groups-and-folding.md) |
| M517 | Storm Translucent Prim Ids | done | - | | E9 |  |  | [M517-storm-translucent-prim-ids.md](PLAN/MILESTONES/M517-storm-translucent-prim-ids.md) |
| M518 | Save Prompt Key Shortcuts | done | - | | E10 | F36 | | [M518-save-prompt-key-shortcuts.md](PLAN/MILESTONES/M518-save-prompt-key-shortcuts.md) |
| M519 | Raw And Loop Toolbar Icons | done | - | | E10 | F36 | | [M519-raw-and-loop-toolbar-icons.md](PLAN/MILESTONES/M519-raw-and-loop-toolbar-icons.md) |
| M520 | USD Toolbar Category And Submenus | done | - | | E10 | F36 | | [M520-usd-toolbar-category-and-submenus.md](PLAN/MILESTONES/M520-usd-toolbar-category-and-submenus.md) |
| M521 | Easier Pipe Grabbing | done | - |  | E10 | F36 | | [M521-easier-pipe-grabbing.md](PLAN/MILESTONES/M521-easier-pipe-grabbing.md) |
| M522 | Geometry Toolbar Category | done | - | | E10 | F15 | | [M522-geometry-toolbar-category.md](PLAN/MILESTONES/M522-geometry-toolbar-category.md) |
| M523 | USDSetAttribute Type From Stage | done | - | | E9 |  | | [M523-usdsetattribute-type-from-stage.md](PLAN/MILESTONES/M523-usdsetattribute-type-from-stage.md) |
| M524 | Vertical Node Style | todo | M351, M522 |  | E10 |  | | [M524-vertical-node-style.md](PLAN/MILESTONES/M524-vertical-node-style.md) |
| M540 | Sparse Tile Field Core | todo | M417 |  | E22 | F35 | | [M540-sparse-tile-field-core.md](PLAN/MILESTONES/M540-sparse-tile-field-core.md) |
| M541 | Pyro Advection And Pressure | todo | M540 |  | E22 | F35 | | [M541-pyro-advection-and-pressure.md](PLAN/MILESTONES/M541-pyro-advection-and-pressure.md) |
| M542 | PyroSolver Node And Sourcing | todo | M541, M404 |  | E22 | F35 | | [M542-pyrosolver-node-and-sourcing.md](PLAN/MILESTONES/M542-pyrosolver-node-and-sourcing.md) |
| M543 | Pyro Fields And Combustion | todo | M542 |  | E22 | F35 | | [M543-pyro-fields-and-combustion.md](PLAN/MILESTONES/M543-pyro-fields-and-combustion.md) |
| M544 | Pyro Shaping Forces | todo | M543 |  | E22 | F35 | | [M544-pyro-shaping-forces.md](PLAN/MILESTONES/M544-pyro-shaping-forces.md) |
| M545 | Pyro Collisions | todo | M543, M418 |  | E22 | F35 | | [M545-pyro-collisions.md](PLAN/MILESTONES/M545-pyro-collisions.md) |
| M546 | PyroSource And Rasterize | todo | M417, M359 |  | E22 | F35 | | [M546-pyrosource-and-rasterize.md](PLAN/MILESTONES/M546-pyrosource-and-rasterize.md) |
| M547 | Pyro Look And Post-Process | todo | M543, M347 |  | E22 | F35 | | [M547-pyro-look-and-post-process.md](PLAN/MILESTONES/M547-pyro-look-and-post-process.md) |
| M548 | Pyro Quick Setups And Checkpoints | todo | M544, M545, M546, M547 |  | E22 | F35 | | [M548-pyro-quick-setups-and-checkpoints.md](PLAN/MILESTONES/M548-pyro-quick-setups-and-checkpoints.md) |
| M554 | Scene Camera For Delegate Renders | doing | M345 | bosley-executor | E9 | F1 | | [M554-scene-camera-for-delegate-renders.md](PLAN/MILESTONES/M554-scene-camera-for-delegate-renders.md) |
| M555 | Dissolve GPU Smaller-Input Mismatch | done | M495 | | E14 |  | | [M555-dissolve-gpu-smaller-input-mismatch.md](PLAN/MILESTONES/M555-dissolve-gpu-smaller-input-mismatch.md) |
| M556 | Scene Graph Bulk Commands | done | - | | E9 | F37 | | [M556-scene-graph-bulk-commands.md](PLAN/MILESTONES/M556-scene-graph-bulk-commands.md) |
| M557 | Scene Graph Purpose Column | done | - | | E9 | F37 | | [M557-scene-graph-purpose-column.md](PLAN/MILESTONES/M557-scene-graph-purpose-column.md) |
| M558 | Scene Graph Expand Commands | doing | M556 | bosley-executor | E9 | F37 | | [M558-scene-graph-expand-commands.md](PLAN/MILESTONES/M558-scene-graph-expand-commands.md) |
| M559 | Native Geometry UAT Round 1 | done | - | | E9 | F15 | | [M559-native-geometry-uat-round-1.md](PLAN/MILESTONES/M559-native-geometry-uat-round-1.md) |
| M560 | USDMerge Opinion Order | todo | - |  | E9 | | | [M560-usdmerge-opinion-order.md](PLAN/MILESTONES/M560-usdmerge-opinion-order.md) |
| M561 | Live Session Personal Viewers | todo | M106 |  | E11 | F5 | | [M561-live-session-personal-viewers.md](PLAN/MILESTONES/M561-live-session-personal-viewers.md) |
| M562 | USDEditPrim Relationship Knobs | todo | - |  | E27 | F38 | | [M562-usdeditprim-relationship-knobs.md](PLAN/MILESTONES/M562-usdeditprim-relationship-knobs.md) |
| M563 | USDSetAttribute Tweak Modes | todo | - |  | E27 | F38 | | [M563-usdsetattribute-tweak-modes.md](PLAN/MILESTONES/M563-usdsetattribute-tweak-modes.md) |
| M564 | USDConfigurePrim Node | todo | M449 |  | E27 | F38 | | [M564-usdconfigureprim-node.md](PLAN/MILESTONES/M564-usdconfigureprim-node.md) |
| M565 | USDConfigureLayer Node | todo | M449 |  | E27 | F38 | | [M565-usdconfigurelayer-node.md](PLAN/MILESTONES/M565-usdconfigurelayer-node.md) |
| M566 | USDCollection Node | todo | M562 |  | E27 | F38 | | [M566-usdcollection-node.md](PLAN/MILESTONES/M566-usdcollection-node.md) |
| M567 | USD Light Linking | todo | M562, M566 |  | E27 | F38 | | [M567-usd-light-linking.md](PLAN/MILESTONES/M567-usd-light-linking.md) |
| M568 | USD Render Geometry Settings | todo | M494 |  | E27 | F38 | | [M568-usd-render-geometry-settings.md](PLAN/MILESTONES/M568-usd-render-geometry-settings.md) |
| M569 | USDInline Node | todo | - |  | E27 | F38 | | [M569-usdinline-node.md](PLAN/MILESTONES/M569-usdinline-node.md) |
| M570 | Scene Query Expression API | todo | - |  | E27 | F38 | | [M570-scene-query-expression-api.md](PLAN/MILESTONES/M570-scene-query-expression-api.md) |
| M571 | Prim Inspector Value Sources | todo | - |  | E27 | F38 | | [M571-prim-inspector-value-sources.md](PLAN/MILESTONES/M571-prim-inspector-value-sources.md) |
| M572 | Render Region | todo | - |  | E27 | F38 | | [M572-render-region.md](PLAN/MILESTONES/M572-render-region.md) |
| M573 | USDLightMixer Node | todo | M520 |  | E27 | F38 | | [M573-usdlightmixer-node.md](PLAN/MILESTONES/M573-usdlightmixer-node.md) |
| M574 | Render Catalogue | todo | M104 |  | E27 | F38 | | [M574-render-catalogue.md](PLAN/MILESTONES/M574-render-catalogue.md) |
| M575 | USD Restructure And Graft | todo | M520, M567 |  | E27 | F38 | | [M575-usd-restructure-and-graft.md](PLAN/MILESTONES/M575-usd-restructure-and-graft.md) |
| M576 | Point Instance Extract And Prune | todo | M520 |  | E27 | F38 | | [M576-point-instance-extract-and-prune.md](PLAN/MILESTONES/M576-point-instance-extract-and-prune.md) |
| M577 | USDConstraint Node | todo | M520 |  | E27 | F38 | | [M577-usdconstraint-node.md](PLAN/MILESTONES/M577-usdconstraint-node.md) |
| M578 | Scene Diff Inspector And Writer | todo | M571, M520 |  | E27 | F38 | | [M578-scene-diff-inspector-and-writer.md](PLAN/MILESTONES/M578-scene-diff-inspector-and-writer.md) |
| M579 | USD Value Clip Sequences | todo | - |  | E27 | F38 | | [M579-usd-value-clip-sequences.md](PLAN/MILESTONES/M579-usd-value-clip-sequences.md) |
| M580 | Scene Load Masks | todo | M520 |  | E27 | F38 | | [M580-scene-load-masks.md](PLAN/MILESTONES/M580-scene-load-masks.md) |
| M581 | Edit Scope Design | todo | - |  | E27 | F38 | | [M581-edit-scope-design.md](PLAN/MILESTONES/M581-edit-scope-design.md) |
| M582 | Look Layers | todo |  |  | E27 | | | [M582-look-layers.md](PLAN/MILESTONES/M582-look-layers.md) |
| M583 | Wedge And Task Dispatch | todo |  |  | E27 | | | [M583-wedge-and-task-dispatch.md](PLAN/MILESTONES/M583-wedge-and-task-dispatch.md) |
| M584 | Layout Instance Painting | todo | M576 |  | E27 | | | [M584-layout-instance-painting.md](PLAN/MILESTONES/M584-layout-instance-painting.md) |
| M585 | USD Asset Gallery | todo |  |  | E27 | | | [M585-usd-asset-gallery.md](PLAN/MILESTONES/M585-usd-asset-gallery.md) |
| M586 | USD Component Builder | todo |  |  | E27 | | | [M586-usd-component-builder.md](PLAN/MILESTONES/M586-usd-component-builder.md) |
| M587 | Physics Drop Layout | todo |  |  | E27 | | | [M587-physics-drop-layout.md](PLAN/MILESTONES/M587-physics-drop-layout.md) |
| M588 | Light Linker Drag And Drop Panel | todo | M567 |  | E27 | | | [M588-light-linker-drag-and-drop-panel.md](PLAN/MILESTONES/M588-light-linker-drag-and-drop-panel.md) |
| M589 | Scene Graph Restructure Commands | todo | M575 |  | E27 | | | [M589-scene-graph-restructure-commands.md](PLAN/MILESTONES/M589-scene-graph-restructure-commands.md) |
| M590 | Non-Modal Notifications | todo | - |  | | | | [M590-non-modal-notifications.md](PLAN/MILESTONES/M590-non-modal-notifications.md) |
| M591 | Native EdgeClamp | todo | M238 |  | E3 | F39 | | [M591-native-edgeclamp.md](PLAN/MILESTONES/M591-native-edgeclamp.md) |
| M592 | Native EdgeSmear | todo | M238 |  | E3 | F39 | | [M592-native-edgesmear.md](PLAN/MILESTONES/M592-native-edgesmear.md) |
| M593 | Native EdgePushVector | todo | M238 |  | E3 | F39 | | [M593-native-edgepushvector.md](PLAN/MILESTONES/M593-native-edgepushvector.md) |
| M594 | Native EdgeDistort | todo | M238 |  | E3 | F39 | | [M594-native-edgedistort.md](PLAN/MILESTONES/M594-native-edgedistort.md) |
| M595 | Native EdgeDetail | todo | M238 |  | E3 | F39 | | [M595-native-edgedetail.md](PLAN/MILESTONES/M595-native-edgedetail.md) |
| M596 | Remove Old User Knob Dialogs | todo | M247 |  | E10 | | | [M596-remove-old-user-knob-dialogs.md](PLAN/MILESTONES/M596-remove-old-user-knob-dialogs.md) |
| M597 | Curve Editor Expanded Knob Rows | todo | - |  | E10 | | | [M597-curve-editor-expanded-knob-rows.md](PLAN/MILESTONES/M597-curve-editor-expanded-knob-rows.md) |
| M598 | Progressive Render GIL Flake | todo | - |  | E14 | | | [M598-progressive-render-gil-flake.md](PLAN/MILESTONES/M598-progressive-render-gil-flake.md) |
| M599 | Runner Efficiency | doing | - | bosley-executor | E2 | | | [M599-runner-efficiency.md](PLAN/MILESTONES/M599-runner-efficiency.md) |
| M600 | Native Geometry UAT Round 2 | doing | - | bosley-executor | E9 | F15 | | [M600-native-geometry-uat-round-2.md](PLAN/MILESTONES/M600-native-geometry-uat-round-2.md) |
| M601 | Every Render A Scheduler Task | todo | M199 |  | E14 | | | [M601-every-render-a-scheduler-task.md](PLAN/MILESTONES/M601-every-render-a-scheduler-task.md) |
| M602 | 3D Viewer Toolbars | todo | M376, M361 |  | E28 | F40 | | [M602-3d-viewer-toolbars.md](PLAN/MILESTONES/M602-3d-viewer-toolbars.md) |
| M603 | Combined Transform Handle | todo | M602 |  | E28 | F40 | | [M603-combined-transform-handle.md](PLAN/MILESTONES/M603-combined-transform-handle.md) |
| M604 | GeoTransform Viewer Handles | todo | M603 |  | E28 | F40 | | [M604-geotransform-viewer-handles.md](PLAN/MILESTONES/M604-geotransform-viewer-handles.md) |
| M605 | GeoEdit Component Transforms | todo | M603 |  | E28 | F40 | | [M605-geoedit-component-transforms.md](PLAN/MILESTONES/M605-geoedit-component-transforms.md) |
| M606 | Viewer Snapping | todo | M603 |  | E28 | F40 | | [M606-viewer-snapping.md](PLAN/MILESTONES/M606-viewer-snapping.md) |
| M607 | Geometry Marker Controls | todo | M602 |  | E28 | F40 | | [M607-geometry-marker-controls.md](PLAN/MILESTONES/M607-geometry-marker-controls.md) |
| M608 | Geometry Group Picker | todo | M602, M359 |  | E28 | F40 | | [M608-geometry-group-picker.md](PLAN/MILESTONES/M608-geometry-group-picker.md) |
| M609 | GeoToUSD Path And Placement | todo | M358 |  | E9 |  | | [M609-geotousd-path-and-placement.md](PLAN/MILESTONES/M609-geotousd-path-and-placement.md) |
| M610 | GeoToUSD Attribute And Material Controls | todo | M609 |  | E9 |  | | [M610-geotousd-attribute-and-material-controls.md](PLAN/MILESTONES/M610-geotousd-attribute-and-material-controls.md) |
| M611 | USDToGeo Source Controls | todo | M358 |  | E9 |  | | [M611-usdtogeo-source-controls.md](PLAN/MILESTONES/M611-usdtogeo-source-controls.md) |
| M612 | USDGeoModify Node | todo | M609, M610, M611, M159 |  | E9 |  | | [M612-usdgeomodify-node.md](PLAN/MILESTONES/M612-usdgeomodify-node.md) |
| M613 | Geometry Node Handles Rollout | todo | M604 |  | E28 |  | | [M613-geometry-node-handles-rollout.md](PLAN/MILESTONES/M613-geometry-node-handles-rollout.md) |

# Epics

| ID | Name | Goal |
|----|------|------|
| E1 | Qt6 Foundation | Land the Linux-only Qt6.8 / C++20 toolchain and dependency baseline. |
| E2 | Build, CI And Release | Make builds fast and CI gating, and ship relocatable releases. |
| E3 | Native Nodes And I/O | Replace OFX-hosted core nodes and I/O with native, metadata-aware ones. |
| E4 | Layers And Channels | Model image data as layers and channels rather than planes. |
| E5 | GPU Compute | Run native node kernels on the GPU with managed placement and residency. |
| E6 | Render Performance | Remove render hotspots and scale the scheduler across cores and memory. |
| E7 | Project Format | Redesign the project file and the serialization, hashing and undo beneath it. |
| E8 | Deep Compositing | Support deep images end to end, from typed edges to filtering. |
| E9 | USD And 3D | Bring USD/Hydra-based 3D scenes and geometry into the node graph. |
| E10 | UI Polish | Polish node graph interaction, menus and visuals. |
| E11 | Live Collaboration | Let several artists edit one graph in a live session. |
| E12 | Graph Diff And Merge | Diff and merge node graphs, with a UI to resolve conflicts. |
| E13 | RFV | Add Reverse-flow Variable feature - arbitrary data that travels up the graph, like frame context. |
| E14 | Architecture | Solidify architecture, considering correctness, performance, robustness, etc. |
| E15 | Loops And I-Nodes | Let a Map drive any effect per pixel, through exact native I-nodes and a Houdini-style loop block over any subgraph. |
| E16 | AI Tools | Bring darktable's ONNX-based AI tools in as scene-linear-safe nodes and a Roto object-select tool, on an on-demand model manager. |
| E17 | darktable Image Tools | Port darktable's scene-referred image modules as native nodes on a shared edge-aware filter library. |
| E18 | Motion Vectors | Generate dense motion vectors natively and move Roto, paint and SplineWarp curves along them. |
| E19 | Utility Renderer | Ship Nucleon Utility, a GPU-native ray-traced Hydra utility renderer with CPU fallback, AOVs, motion blur, subdivision and deep output. |
| E20 | OpenNucleon Rebrand | Rename the project OpenNucleon and the app Nucleon throughout, with new logo, splash and icons. |
| E21 | Nucleon 1.0 Release | Ship Nucleon 1.0 with a public landing site, versioned online and bundled reference docs, release notes and an honest versioning and beta channel. |
| E22 | Simulation And Solvers | Loop, solve and simulate native geometry and images: For-Each and Solver blocks, a POP-style particle system, and 2D fluids. |
| E23 | Overlay Flow | Show node overlays wherever their image goes: off the render path, deformed through warps, and on projected or textured geometry. |
| E24 | Lens Optics | Trace real lens prescriptions for physically based bokeh and lens flares from one shared optics library, lens database and aperture model. |
| E25 | Kernel Networks | Build kernel code as node graphs: geometry, particle and image VOPs on the generic port-network core, plus an image wrangle. |
| E26 | Channel Networks | Drive animation from channel networks: cooked channel graphs exported onto knobs and shapes, with sources and animation layers. |
| E27 | Scene Authoring And Lighting | Close the feature-gap survey: prim, collection and light-link authoring, render attributes and regions, light mixing, render catalogue, restructure and instance edits, and scene inspection. |
| E28 | 3D Viewer Tools | Edit scenes and native geometry directly in the 3D viewer: tool, display and top toolbars, transform handles, snapping, marker controls and group pickers. |

# Features

| ID | Name | Status | Owner | Needs | Branch | Goal |
|----|------|--------|-------|-------|--------|------|
| F1 | VDB Volumes | open |  |  | feature/f1-vdb-volumes | Bring a VDB into the scene, shade it with one material across Storm, Cycles and MoonRay, and inspect it in the viewer. |
| F2 | Native Shader Networks | open |  |  | feature/f2-native-shader-networks | Build, tweak and render each renderer's native shading networks from one builder node, with full or nearly full coverage of Cycles and MoonRay. |
| F3 | Cycles And MoonRay Delegates | landed | misty-marten |  | feature/f3-cycles-and-moonray-delegates | Bundle Cycles and MoonRay and render through them in USDRender and the viewer, with full render settings, tested on one AppImage. |
| F4 | Heavy Scene Viewer | landed |  |  | feature/f4-heavy-scene-viewer | Open and inspect production-sized USD scenes without blocking, with deferred loading, draw states and full prim inspection. |
| F5 | Live Sessions V1 | open |  |  | feature/f5-live-sessions-v1 | Let several artists edit one graph live, with presence, per-user undo and viewer following. |
| F6 | Unified Tiered Cache | landed | bosley-executor | | feature/f6-unified-tiered-cache | Reuse unchanged upstream work across RAM, disk and VRAM so a downstream edit reaches the viewer within budget, and show truthfully what is rendering. |
| F7 | Natron Utility Core | shipping | bosley-executor |  | feature/f7-natron-utility-core | A usable Nucleon Utility renderer on CPU and GPU, with materials, lights, sampling, reflection, subdivision and projection modes. Ships only after F9 - Delegate Projection lands, for M364 - Delegate Cancel Responsiveness. |
| F8 | Motion Vectors | landed | | | feature/f8-motion-vectors | Generate dense motion vectors classically, on the GPU and with a neural model, and move Roto and SplineWarp curves along them. |
| F9 | Delegate Projection | landed | bosley-native-nodes |  | feature/f9-delegate-projection | Live textures and camera projection in Cycles and MoonRay, with per-placement and deforming-geometry projection. |
| F10 | Viewer Polish | landed | bosley-native-nodes |  | feature/f10-viewer-polish | Ship a batch of small viewer and properties-panel fixes on one build and one UAT. |
| F11 | Texture Painting | open |  |  | feature/f11-texture-painting | Paint directly on meshes in the 3D view with UDIM textures, layering over existing materials, baking and paint-through inputs, including painting native geometry with its own UV operators, attribute painting, and a Viewer 2D/3D switch. |
| F12 | Expression Speed | open | bosley-executor |  | feature/f12-expression-speed | Benchmark Python knob expressions and make them cheap on large graphs, through a lighter interpreter path, a result cache and a native evaluator. |
| F13 | ZDefocus Rework | open | bosley-executor |  | feature/f13-zdefocus-rework | Measure ZDefocus against ground truth, add traced lenses and debug outputs, match or exceed Higx Optics' traced bokeh, and revisit its engine from the results. |
| F14 | Map Blend I-Nodes | shipping | bosley-executor |  | feature/f14-map-blend-i-nodes | Turn any subgraph into a Map-driven I-node through LoopEnd's Map blend and one Make I-Node action. |
| F15 | Native Geometry | shipping | bosley-executor |  | feature/f15-native-geometry | A SOP-style native geometry context: a Houdini-like detail, bridges to USD, operators, Python and VEX-like wrangles, point editing and a geometry spreadsheet. |
| F16 | User Knob Editor | shipping | bosley-executor |  | feature/f16-user-knob-editor | Create and manage user knobs in place on the properties panel, with type changes, undo, button scripts, drag-in links and copy/paste. |
| F17 | OpenNucleon Flag Day | open |  |  | feature/f17-opennucleon-flag-day | Rename Natron to OpenNucleon/Nucleon throughout, code included, with new art and the GPLv3 relicence, landed in one switchover at a quiet time the user chooses. |
| F18 | Geometry Loops And Solver | open |  |  | feature/f18-geometry-loops-and-solver | Loop and solve native geometry: For-Each blocks on render contexts, a native geometry file cache, and a Solver SOP with a sim cache. |
| F19 | Particles | open |  |  | feature/f19-particles | A POP-style particle system on native geometry, with sources, forces, interaction, collisions, a GPU path and 2D image particles. Waits for F22 - Native VDB to land, for collisions. |
| F20 | 2D Fluids | open |  |  | feature/f20-2d-fluids | Grid smoke and fire simulated in image space from image sources and motion vectors, with a shading node. |
| F21 | Geometry Operators | open |  |  | feature/f21-geometry-operators | Houdini SOP operator batches: modeling, advanced modeling, attribute, deformer and time ops, PolyExtrude/Boolean/Fuse, and a Bezier curve primitive. |
| F22 | Native VDB | open |  |  | feature/f22-native-vdb | An OpenVDB volume primitive in native geometry, bridged to USD, with conversion, SDF ops and a volume wrangle. |
| F23 | Deep Nodes | open |  |  | feature/f23-deep-nodes | Native deep filtering, transform, inspection and tier-2 nodes on the existing deep transport: OpenDCX DeepBlur and DeepTransform, DeepSample and a chosen DeepC-style set. |
| F24 | Deep Rendering | open |  |  | feature/f24-deep-rendering | Render true deep images in process from Nucleon Utility, Cycles and MoonRay through one host-side deep render buffer, with DeepCompact. |
| F25 | Overlay Flow | open |  |  | feature/f25-overlay-flow | Overlays deform through every warp and onto projected geometry, with CardProject25D's 2.5D projection carrying them through. |
| F26 | MaterialX Rendering | open |  |  | feature/f26-materialx-rendering | Render MaterialX materials in Cycles and MoonRay through native translation, with .mtlx input and a USDOpenPBR lookdev node. |
| F27 | USD Layer Authoring | open |  |  | feature/f27-usd-layer-authoring | Control which USD layers an export writes: layer breaks for delta-only files, named and nested layer files from one USDWrite, and a live preview of what will be written. |
| F28 | Colour Warp | open | bosley-executor |  | feature/f28-colour-warp | A ColorWarp node: pick colours in the viewer and push them through colour space like warp pins, with a 3D colour-space view. |
| F29 | UI Polish Batch | landed |  |  | feature/f29-ui-polish-batch | Ship five small UI fixes — snapped viewer zoom, node error and warning display, Roto insert-point drag, keyframe markers for visible pages and the file dialog toolbar — on one build and one merge. |
| F30 | Lens Optics Base | landed | | | feature/f30-lens-optics-base | One optics base for traced bokeh and lens flares: a shared lens library, the bundled lens database and browser, and a shared aperture model. |
| F31 | Lens Flares | open | bosley-executor |  | feature/f31-lens-flares | A physically based, ray-traced LensFlare node with ghosts, starburst, layers and USD lights on CPU and GPU, with lens browser previews. Waits for F30 - Lens Optics Base to land. |
| F32 | Channel Networks | open |  |  | feature/f32-channel-networks | CHOP-style channel networks that drive knobs and shapes, with operators, Channel VOP, every source and channel-based animation layers. |
| F33 | Graph Diff And Merge | shipping | bosley-executor |  | feature/f33-graph-diff-and-merge | Diff and merge two versions of a graph, with a resolution UI, accepted in one UAT. |
| F34 | Scene Lens Distortion | open |  |  | feature/f34-scene-lens-distortion | Camera lens distortion carried in the USD scene, authored on cameras and used by LensDistortion and the projection nodes. |
| F36 | UI Polish Batch II | shipping | bosley-executor |  | feature/f36-ui-polish-batch-ii | Ship four small UI fixes — one-key answers in the save prompt, Raw and Loop toolbar icons, a USD toolbar category with functional submenus and easier-to-grab pipes — on one build and one merge. |
| F35 | Pyro | open |  |  | feature/f35-pyro | A sparse-tile smoke and fire solver on native VDB volumes, with sourcing, shaping, collisions, look baking and quick setups, on GPU and CPU. Waits for F18 - Geometry Loops And Solver and F22 - Native VDB to land. |
| F37 | Scene Graph Bulk Ops | open |  |  | feature/f37-scene-graph-bulk-ops | Load, draw and expand whole Scene Graph branches in one action, by row or down to a kind, with a Purpose column. |
| F38 | Scene Authoring And Lighting | open |  |  | feature/f38-scene-authoring-and-lighting | The survey-driven scene authoring and lighting set (M562–M581): relationship, tweak, metadata and layer editing, collections, light linking and mixing, render attributes and regions, catalogue, restructure, instances, constraints, queries, inspection and load masks. |
| F39 | Edge Extend Family | open |  |  | feature/f39-edge-extend-family | Native edge-extend nodes (EdgePush, EdgeClamp, EdgeSmear, EdgePushVector, EdgeDistort) on a shared fill kit, plus EdgeDetail for restoring detail. |
| F40 | 3D Viewer Tools | open |  |  | feature/f40-3d-viewer-tools | Three-bar 3D viewer toolbars, a combined transform handle, geometry handles and component transforms, snapping, marker controls and group pickers, accepted in one UAT. |

# Open questions
- **M104 - Merge Resolution UI (2026-11-22):** Keep Both Keys puts theirs' key one frame after mine, or halfway to the next key when that's closer. Confirm, or give the rule you want.

- **Engine TLS race (found in M402, 2026-11-22):** `makeInheritedTLSData` (`Engine/TLSHolderImpl.h:70`, HostFrameThreading path via `inheritFromSpawner`/`getTLSData`) copies a live spawner thread's `frameArgs` list without a lock while the spawner may push/pop entries (`ContextFrameArgsScope::install`, `Engine/ParallelRenderArgs.cpp:1862`), a use-after-free that segfaulted M402's 100k-iteration loop. M402 fixed its own path (helpers no longer inherit); any other `SpawnedThreadScope` user whose spawner keeps cooking is exposed, and it may explain unexplained render-thread crashes/flakes. File an E14 milestone to make the inheritance safe (lock, or snapshot at spawn time)?

- **F4 - Heavy Scene Viewer post-merge UAT (user, 2026-11-22):** merged as 77d7fb709 before the final UAT, at the user's request; the user runs `build/appimages/Natron-f4-heavy-scene-viewer.AppImage` with its steps `.md` later and files follow-ups for any issue.

- **M105 - Live Session Core (2026-11-22):** a value change to a user knob is sent as a whole `SetUserKnobsOp` (`GraphEditRecorder.cpp:~385`), which on every peer deletes and rebuilds all user knobs and their panel page (`GraphOpApply.cpp:431-451`), so peers flicker on each user-knob edit. A per-value op would fix it: in M105's scope, or a follow-up?

- **M402 - Geometry Loops (2026-11-22, 624858508):** design calls to confirm: (1) the hidden piece variable holds the piece's value (attribute value or piece number) and each Begin extracts from its own input; the content-hash registry is kept only for Feedback; (2) Stop Condition is a Python expression over loop variables, evaluated before any cook (can't read geometry), while Stop Attribute is a feedback-only detail attribute that can stop on the result; (3) zero iterations under Feedback output empty geometry, not the input; (4) a Begin viewed outside a loop cook previews the Single Pass piece or first piece; (5) as with image LoopEnd, a stop condition reading `loop.*` can make the End re-cook when an outer loop's `loop.*` changes. 10,000 pieces means 10,000 cache entries for Begin and body; the bench will show the cost.

- **M408 - Kernel Language GPU Backend (2026-11-22):** wrangles go to the GPU whenever one is available; the weight-based placement (M239) has no hook for geometry work. Add one, or a minimum element count below which details stay on the CPU? Also: noise/matrix/quaternion run single-precision on the GPU (expected ~1e-7 vs the CPU's double, test tolerance 1e-5); the CPU-fallback info message shows only when a GPU is present.

- **M403 - Geometry File Cache (2026-11-22, 3540fd5d5):** (1) the Version knob is substituted into the path as `$VER` / `$VER3` (3-padded); confirm or pick another syntax; (2) rendering in Load From Disk or Pass Through mode fails with a message so a render can't overwrite a cache being loaded, rather than always writing the input; (3) files are memory-mapped but each frame is decoded whole on read (no lazy pages).

- **M417 - VDB Volume Primitive (2026-11-22, 65f09ce71):** written `<disk cache>/vdb/<hash>.vdb` field files aren't tied into session-cache eviction (no hook found); the GUI check needs a `Tests/fixtures/m417-fog-sphere.vdb` generated after the build; GeoFileFormat embedding of VDB waits for M403.


- **M494 - Delegate Subdivision (2026-11-22):** (1) the CyclesAovs bilinear branch can't see the delegate toggle, so the `refineLevel` branch and its dirty locator were removed instead; (2) Cycles' Loop-scheme warning reaches only the console, not the node message, unless the static stage scan gains a Loop check; (3) MoonRay's dicing camera uses a dedicated `natron:dicingCamera` setting (the `dicing_camera` scene variable holds an object); moving the camera doesn't re-dice in Cycles; (4) Natron rebuilds the engine on any setting change, so the forks' live re-sync is never exercised. Fork pins: Cycles 18a83a2 (branch natron/m494-subdivision), openmoonray ee4955e (branch natron/m494-subdivision, off the old 9cc77b5 pin, independent of M460's c4edab6; whichever lands second combines both).

- **M362 - Geometry Operators II (planner, 2026-11-22):** five questions in the milestone file's `## Open questions`: Boolean core (in-house vs libigl+CGAL vs Manifold), the source of authoritative defaults, Attribute Create's local variables / info block, kernel `d@` dicts, and where repeated knob blocks live.

- **M460 - MoonRay True Deep Patch (2026-11-22):** P2.T1 (deep AOVs and volumes behind presence) needs decisions: how min/max/closest-filtered AOVs split per presence layer; whether a 50% presence card clips the fog behind it; how volume deep segments accumulate across progressive passes. It also needs a MoonRay compile-and-render loop a no-build lane can't run. Separately, the intro's "lowers the deep Z tolerance default" has no task: MoonRay's scene-variable default, or Natron sending a smaller value?

- **M518 - Save Prompt Key Shortcuts (2026-11-22):** T2's verify can't run as written: no caller raises a Yes/No question through Natron's own MessageBox (the auto-save prompt uses the stock dialog). The GUI check covers `&OK` on MessageBox and y/n on the stock question dialog instead.

- **M523 - USDSetAttribute Type From Stage (2026-11-22):** the type-mismatch warning covers authored non-schema attributes only; schema attributes (e.g. `radius`) keep the existing schema-mismatch render error, as the Edge cases say. The type lookup runs `evaluateScene` on a worker thread, with no off-thread precedent; the review checks that.

- **F15 - Native Geometry (PM, 2026-11-22):** main merged into F15 (8bd000af4, gate green); geometry keeps its own RAM cache beside F6's unified tiered cache. Folding it into the node cache (as deep was) is the alternative. F18, F19, F21 and F22 now stack on F15 and can't ship before it.
- **M365 - Delegate Display Transform (user, 2026-11-22):** apply the camera's exposure in the viewer when looking through a camera; add a toggle button to the right of the viewer's exposure (gain) slider to turn it on/off, and a non-editable label next to it showing the camera's exposure value. Screenshots to re-send after.

- **F7 - Natron Utility Core UAT round 1 (user, 2026-11-21):**
  1. No reflective or refractive materials in this build.
  2. Performance is so-so: what can be done?
  3. Motion blur: shouldn't need many samples to avoid steppy blur. Any enabled motion blur interpolates between temporal samples by shutter segments; the default is 1 segment (previous frame → this one → next), with rays distributed in time.
  4. Crashed on an AMD card rendering Island.
  5. Island failed on the AMD GPU host: more instances than the 2^24 a ray query can tell apart.
  6. Consider a GPU shader or rasteriser renderer for maximum speed in simple setups (mostly projections with motion blur); to discuss, probably a follow-up. Filed as M514 - Utility Raster Mode and M515 - Storm Oversample And Motion Blur (cat-plan, 2026-11-22).

  Fixes for 1–5 are being investigated on the F7 branch. **PM, 2026-11-21:** PR #316 is green on 8bc8553a3 after fixing an llvmpipe one-past-the-end structure-table read in the GPU tracer (a zero sentinel entry), two wrong test expectations and a GPU tolerance; main merged in 8de8d85c7. On the branch since the round-1 AppImage: USDPreviewSurface specular/clearcoat/opacity/IOR inputs (1) and shutter-segment motion blur (3). Item 5 was the round-1 AppImage's own check; a53d277e6 already splits instances across top-level structures. f09a5d634 hardens the GPU path (allocation-limit and VRAM-budget checks, Unsupported falls back to CPU under Auto, slow tracer submissions shrink); the production WavefrontGpu batch sizes are still fixed, pending AMD logs. **Round-2 AppImage** `build/appimages/Natron-f7-natron-utility-core.AppImage` (release, f09a5d634, PR #316 green) with its .md "Round 2" section; Needs uat.
- **F29 - UI Polish Batch (PM, 2026-11-20):** Feature review round 1 is fixed (f75950a65), and the m421 script's graph lookup is made robust (1c60da022). The gate is green (release, ctest, 8 GUI scripts). The round-2 UAT AppImage is `build/appimages/Natron-f29-ui-polish-batch.AppImage`, with its .md (see "Round 2+").

- **M472 - GPU Raster Pipelines post-merge check (PM, 2026-11-20, merged c97bbfcb6 / #318):** record the raster-capable record context's capability report on RADV and NVIDIA on the GPU host (only llvmpipe has run it).





- **M272 - Script Writes To Selected Roto Shapes (2026-11-14):** (1) Should Python get `Roto.selectItems()` / `getSelectedItems()`? PM default: no, out of scope. (2) With several shapes selected, should an expression set on the node knob be copied to each selected shape (PM default, as cloning does today) or stay on the node knob only? Work proceeds on the defaults.
- **Nucleon 1.0 must-land list (user, 2026-11-10: "I'll decide later"):** which Features and milestones must be on `main` before 1.0. M396 - Release 1.0 Cut is held, not eligible for pickup, until the user names them; they are then added to its `Deps`.
- **Site repo setup for E21 - Nucleon 1.0 Release (user action, after the `opennucleon` org exists):** in `opennucleon/opennucleon.github.io`, enable Pages from the `gh-pages` branch, add a deploy key with write access, and store its private half as the OpenNucleon secret `SITE_DEPLOY_KEY`. M392 - Docs Publishing needs this. Once M388 - Docs Site Scaffold merges, add its `Docs` job to `main`'s required checks.
- **M382 - Nucleon Logo And Artwork (PM, 2026-11-10):** pick one of the four logo concepts (nucleus, orbital, N monogram, node graph), sent as one PNG per concept and background, with notes for the first refinement round.
- **F17 - OpenNucleon Flag Day is held (user, 2026-11-10):** only M382 - Nucleon Logo And Artwork may start now, and it merges into the Feature branch only. M383 - Rename Script And Guard, M384 - Plugin Forks Rename, M157 - Relicense To GPLv3 Or Later, M385 - OpenNucleon Code Rename, M386 - Nucleon Branding And Packaging and M387 - Dev Infrastructure Rename are not eligible for pickup until the user names the switchover time. Before that, the user creates the `opennucleon` GitHub org, transfers and renames the repo (`opennucleon/OpenNucleon`) and the forks (`openfx-{io,misc,arena,supportext}`, `openfx-natron` → `openfx-nucleon`), and creates `opennucleon.github.io`. After the landing: every open branch merges main and runs `tools/rename/rename.py --changed-since origin/main`, and `/cat-plan` sweeps the `todo` milestone briefs through the same script.
- **M276 - Upstream Reuse On Edits (PM, 2026-11-06):** the latency baseline and GPU counts need your host: `build/appimages/Natron-m276-upstream-reuse-on-edits.AppImage` with the steps in its `.md`.
- **F3 - Cycles And MoonRay Delegates, Cycles path guiding (PM, 2026-11-06):** Path Guiding isn't compiled into Cycles; build Cycles with openpgl only if you want guiding. MoonRay's denoiser is now M513 - MoonRay OIDN Denoiser.
- **NatronRenderer and [Project] (cat-plan, 2026-11-22):** a code trace on main says this is fixed: an .ntpz sets the [Project] row to the bundle folder (`Engine/ProjectPrivate.cpp:209-216`), and Write paths expand variables since M245 - Write Path Variables. No test runs the NatronRenderer binary on it, so re-run `NatronRenderer -w Write 3-3 defocus_retime.ntpz` once to confirm; file a fix only if it still fails.
- **Post-merge checks for the user (user, 2026-11-02):** these merged on approvals given against AppImages that predated their round-1 fixes; test the listed behaviour on main once merged. M228 - 3D Viewer Navigation (camera knobs as GUI edits, Storm GL lock), M229 - 3D Viewer Guides And Display (guide-size preference, sized light guides, number-knob layout), M230 - 3D Transform Widgets (VkResult in the GPU error), M245 - Write Path Variables (path variables on project RFVs, dialog, rename rewrite, undo). Batch merge of 2026-11-05 (lanes combined after their UAT builds; test on main): Keyer and ChromaKeyer with the M95 - Knob Cleanup And Keyer Redesign behaviour under the plain IDs from M111 - Native Node Identity Cleanup; Radial and the other shapes from M128 - Native Shape Generators have no Interactive Update or HiDPI knobs; ErodeDilate and M186 - Exact IDefocus together; M160 - Lensfun Lens Correction and M233/M234 live textures and projection under plain plugin IDs; the three `tools/bench/comps/*.ntpz` comps were edited by script and not loaded in Natron: load them, or regenerate with `COMP=keying_grade COMPS_OUT=tools/bench/comps NatronRenderer -b tools/bench/comps/build_comps.py`, once per COMP. M118 - Large Graph GUI Responsiveness (merged 2026-11-12, PR #259): on the laptop, wheel zoom in the node graph pivots on the cursor. M116 - GPU Region Overlay (merged 2026-11-12, PR #261; approved before its review fixes): with the overlay on, drag a region member (only its outline follows), zoom out to low detail and toggle the overlay (faded nodes fade and unfade), select all (no flicker); the tooltip now says it fades nodes that computed no pixels.
- **M206 - Neural Raw Denoise:** remind the user later about the hand-made darktable 5 reference export (user, 2026-11-02: "remind me later").



- **M123 - Native Sharpen And Glow, Bloom design:** the user requires GPU residency (no CPU fallback) if at all possible; candidates are being spiked. Filter default (FIR Gaussian, recommended, or CImg's quasi-Gaussian) still open.
- **M198 - RenderEngine On Node, M199 - Remove Legacy Scheduler, M200 - Folded Knob Shared Storage, M201 - Trimap Removal:** do not start without the user's go-ahead (user, 2026-10-25, M31 triage). Each stub's `Blocked on:` names what the user is waiting for. M202 - Clean-Hash Cache Stores needs no go-ahead.
- **M46 - Labelmaker Annotations:** on hold, not eligible for pickup (user, 2026-10-27, during its grilling). Resume only with the user's go-ahead, then finish the interview (`/cat-grill M46`).
- **M187 - Variable Sharpen And Glow:** on hold, not eligible for pickup (user, 2026-10-27, inbox). Resume only with the user's say-so.
- **M188 - Variable Median And DirBlur:** on hold, not eligible for pickup (user, 2026-10-27, inbox). Resume only with the user's say-so.
- **M68 - Headless GL EGL:** deferred, not eligible for pickup without the user's go-ahead (user, 2026-10-05). Its `Deps` were filled 2026-11-14, so this line is now what holds it.
- **M21 - Deep Tier-2 Nodes (cat-plan, 2026-11-14):** which DeepC-style nodes make the cut (DeepCGrade-alike, position and normal mattes, DeepSlice, DeepHoldout variants)? The stub cannot be elaborated until the node list is chosen.
- **M174 - Wayland Support:** blocked until the user approves starting it (user, 2026-10-24). Stub; elaborate on approval.


- **M64 - Tiled Rendering:** dropped for now (user, 2026-10-08): this 4-core dev host is too slow for the Phase 64.7 re-evaluation benchmarks to be meaningful. Resume only on a faster host and with the user's go-ahead.
- **M166 - Non-Float Plane Model:** the user wants to rethink the model before it is planned: Read emitting EXR half/uint32 layers natively, and possibly a bit depth per layer. Settle the framing in `/cat-discuss`, then elaborate.





- **F9 - Delegate Projection post-merge checks (PM, 2026-11-15, landed f74a781e2 / #282):** the UAT was approved before the main merge and the Cycles restart-detection rework (fork reset counters, cancel before a pass keeps the engine). On main, please re-check: a file reload during a Cycles progressive render shows the reloaded scene; editing a Cycles scene that has finished refines in place without a pause; MoonRay edits restart promptly; cancelling during a render stays fast.


- **F8 - Motion Vectors ship (PM, 2026-11-15):** gate green on c22092d9a (run 43749309288; one unrelated MoonRay timeout passed on re-run). Feature PR #295 opened; Codex Feature review round 1 (3 findings: Translate fit under layer transforms, Max Inference Size knob, follow-worker TLS) fixed in 1b3bca54e; PR CI and a release GUI gate running. Release GUI gate green on 1b3bca54e (run 43775176418, 6 follow/vector scripts); UAT AppImage build/appimages/Natron-f8-motion-vectors.AppImage with steps .md handed over (GPU-host timings dropped per the no-timing-gates rule). UAT round 1 (user, 2026-11-16) asked for 9 changes:
  - keyframe reduction becomes a separate post-process on any shape;
  - a Vectors input replaces the source dropdown;
  - the follow toolbar is hidden unless Vectors is connected;
  - the confidence label is on the wrong toolbar;
  - a stray node-name label is removed;
  - follow fails when keys already exist;
  - a follow range forward/backward option is added;
  - follow buttons lose their text labels;
  - a separate follow animation layer (follow-up, inbox).
  The first eight are being fixed on the Feature branch in three batches; Needs stays uat.
