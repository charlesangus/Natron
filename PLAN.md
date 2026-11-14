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
| M79 | Layered Document Readers | blocked | M75 | bosley-native-nodes | E3 | | response | [M79-layered-document-readers.md](PLAN/MILESTONES/M79-layered-document-readers.md) |
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
| M68 | Headless GL EGL | todo |  |  | E2 | | | [M68-headless-gl-egl-backend.md](PLAN/MILESTONES/M68-headless-gl-egl-backend.md) |
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
| M330 | Graph Draw Policy | doing | M329 | bosley-native-nodes | E9 | F4 | | [M330-graph-draw-policy.md](PLAN/MILESTONES/M330-graph-draw-policy.md) |
| M331 | Deferred Pattern Matching | done | M52, M327, M328 | bosley-native-nodes | E9 | F4 | | [M331-deferred-pattern-matching.md](PLAN/MILESTONES/M331-deferred-pattern-matching.md) |
| M332 | Scene Graph Points | done | M328 | bosley-native-nodes | E9 | F4 | | [M332-scene-graph-points.md](PLAN/MILESTONES/M332-scene-graph-points.md) |
| M333 | Prim Inspector | done | M232 | misty-marten | E9 | F4 | | [M333-prim-inspector.md](PLAN/MILESTONES/M333-prim-inspector.md) |
| M334 | Knob Prim Tools | doing | M52, M232, M331 | bosley-native-nodes | E9 | F4 | | [M334-knob-prim-tools.md](PLAN/MILESTONES/M334-knob-prim-tools.md) |
| M335 | Viewer Render Draw States | doing | M254, M329 | bosley-native-nodes | E9 | F4 | | [M335-viewer-render-draw-states.md](PLAN/MILESTONES/M335-viewer-render-draw-states.md) |
| M336 | Heavy Scene Acceptance | todo | M329, M330, M331, M332 |  | E9 | F4 | | [M336-heavy-scene-acceptance.md](PLAN/MILESTONES/M336-heavy-scene-acceptance.md) |
| M401 | 3D Viewer Breadcrumbs | done | M231, M232, M328, M332 | bosley-native-nodes | E9 | F4 | | [M401-3d-viewer-breadcrumbs.md](PLAN/MILESTONES/M401-3d-viewer-breadcrumbs.md) |
| M423 | USDPrune Isolate Mode | todo | M381 |  | E9 | F4 | | [M423-usdprune-isolate-mode.md](PLAN/MILESTONES/M423-usdprune-isolate-mode.md) |
| M337 | Geometry Spreadsheet | todo | M54, M358, M361, M333 |  | E9 | F15 | | [M337-geometry-spreadsheet.md](PLAN/MILESTONES/M337-geometry-spreadsheet.md) |
| M233 | Live Node Textures | done | M19, M20 | bosley-native-nodes | E9 | | | [M233-live-node-textures.md](PLAN/MILESTONES/M233-live-node-textures.md) |
| M234 | Camera Projection Nodes | done | M227, M233 | bosley-native-nodes | E9 | | | [M234-camera-projection-nodes.md](PLAN/MILESTONES/M234-camera-projection-nodes.md) |
| M235 | USDCamera From File | done | M226 | bosley-native-nodes | E9 | | | [M235-usdcamera-from-file.md](PLAN/MILESTONES/M235-usdcamera-from-file.md) |
| M236 | Shared Materials And Assignment | done | M226 | bosley-native-nodes | E9 | | | [M236-shared-materials-and-assignment.md](PLAN/MILESTONES/M236-shared-materials-and-assignment.md) |
| M237 | CI Flake Fixes | done | - | bosley-native-nodes | E2 | | | [M237-ci-flake-fixes.md](PLAN/MILESTONES/M237-ci-flake-fixes.md) |
| M238 | EdgeExtend Options | blocked | M123 | bosley-native-nodes | E3 | | response | [M238-edgeextend-options.md](PLAN/MILESTONES/M238-edgeextend-options.md) |
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
| M255 | Delegate Output Spikes | blocked | M251, M252 | bosley-native-nodes | E9 | | | [M255-delegate-output-spikes.md](PLAN/MILESTONES/M255-delegate-output-spikes.md) |
| M256 | Delegate AOV Selection | todo | M255 |  | E9 | | | [M256-delegate-aov-selection.md](PLAN/MILESTONES/M256-delegate-aov-selection.md) |
| M257 | Cycles Deep Output | todo | M255 |  | E8 | | | [M257-cycles-deep-output.md](PLAN/MILESTONES/M257-cycles-deep-output.md) |
| M258 | MoonRay Deep Output | todo | M255 |  | E8 | | | [M258-moonray-deep-output.md](PLAN/MILESTONES/M258-moonray-deep-output.md) |
| M259 | GPU Placement Under Render Contexts | done | M180, M181, M182 | bosley-native-nodes | E15 | | | [M259-gpu-placement-under-render-contexts.md](PLAN/MILESTONES/M259-gpu-placement-under-render-contexts.md) |
| M52 | USD Layer-Stack Completion | done | M226 | bosley-native-nodes | E9 | | | [M52-usd-layer-stack-completion.md](PLAN/MILESTONES/M52-usd-layer-stack-completion.md) |
| M53 | Projected Texture Painting | doing | M52, M233, M311 | bosley-native-nodes | E9 | F11 | | [M53-projected-texture-painting.md](PLAN/MILESTONES/M53-projected-texture-painting.md) |
| M54 | Native Geometry Data Model | done | M52, M227 | bosley-native-nodes | E9 | F15 | | [M54-sops-style-geometry.md](PLAN/MILESTONES/M54-sops-style-geometry.md) |
| M358 | Geometry Bridges And Display | done | M54, M52 | | E9 | F15 | | [M358-geometry-bridges-and-display.md](PLAN/MILESTONES/M358-geometry-bridges-and-display.md) |
| M359 | Geometry Operators | done | M54 | | E9 | F15 | | [M359-geometry-operators.md](PLAN/MILESTONES/M359-geometry-operators.md) |
| M360 | Geometry Kernel Language | done | M54 | | E9 | F15 | | [M360-geometry-kernel-language.md](PLAN/MILESTONES/M360-geometry-kernel-language.md) |
| M361 | Geometry Point Editing | todo | M358, M359, M360, M376 |  | E9 | F15 | | [M361-geometry-point-editing.md](PLAN/MILESTONES/M361-geometry-point-editing.md) |
| M362 | Geometry Operators II | todo | M359, M360 |  | E9 | F21 | | [M362-geometry-operators-ii.md](PLAN/MILESTONES/M362-geometry-operators-ii.md) |
| M363 | Geometry Display Extras | todo | M358, M376 |  | E9 |  | | [M363-geometry-display-extras.md](PLAN/MILESTONES/M363-geometry-display-extras.md) |
| M402 | Geometry Loops | todo | M180, M181, M190, M359, M360 |  | E22 | F18 | | [M402-geometry-loops.md](PLAN/MILESTONES/M402-geometry-loops.md) |
| M403 | Geometry File Cache | todo | M54, M359 |  | E22 | F18 | | [M403-geometry-file-cache.md](PLAN/MILESTONES/M403-geometry-file-cache.md) |
| M404 | Geometry Solver | todo | M402, M403 |  | E22 | F18 | | [M404-geometry-solver.md](PLAN/MILESTONES/M404-geometry-solver.md) |
| M405 | Particle Solver Core | todo | M404, M359 |  | E22 | F19 | | [M405-particle-solver-core.md](PLAN/MILESTONES/M405-particle-solver-core.md) |
| M406 | Particle Forces And Wrangle | todo | M405, M360 |  | E22 | F19 | | [M406-particle-forces-and-wrangle.md](PLAN/MILESTONES/M406-particle-forces-and-wrangle.md) |
| M407 | Particle Collisions | todo | M405, M418 |  | E22 | F19 | | [M407-particle-collisions.md](PLAN/MILESTONES/M407-particle-collisions.md) |
| M408 | Kernel Language GPU Backend | todo | M360 |  | E22 | F19 | | [M408-kernel-language-gpu-backend.md](PLAN/MILESTONES/M408-kernel-language-gpu-backend.md) |
| M409 | Particle GPU Path | todo | M406, M407, M408 |  | E22 | F19 | | [M409-particle-gpu-path.md](PLAN/MILESTONES/M409-particle-gpu-path.md) |
| M412 | 2D Image Particles | todo | M405, M406 |  | E22 | F19 | | [M412-2d-image-particles.md](PLAN/MILESTONES/M412-2d-image-particles.md) |
| M410 | 2D Fluid Solver Core | todo | M190 |  | E22 | F20 | | [M410-2d-fluid-solver-core.md](PLAN/MILESTONES/M410-2d-fluid-solver-core.md) |
| M411 | 2D Fluid Fire And Controls | todo | M410 |  | E22 | F20 | | [M411-2d-fluid-fire-and-controls.md](PLAN/MILESTONES/M411-2d-fluid-fire-and-controls.md) |
| M420 | Spline Curve Primitive | todo | M358, M376 |  | E9 | F21 | | [M420-spline-curve-primitive.md](PLAN/MILESTONES/M420-spline-curve-primitive.md) |
| M421 | Stroke Keyframe Indicators | doing | M261, M305 | bosley-native-nodes | E10 | | | [M421-stroke-keyframe-indicators.md](PLAN/MILESTONES/M421-stroke-keyframe-indicators.md) |
| M424 | Python setExpression Refreshes The Viewer | doing | - | bosley-native-nodes | E10 | | | [M424-python-set-expression-refreshes-viewer.md](PLAN/MILESTONES/M424-python-set-expression-refreshes-viewer.md) |
| M422 | Snap Viewer Wheel Zoom | todo | - |  | E10 | | | [M422-snap-viewer-wheel-zoom.md](PLAN/MILESTONES/M422-snap-viewer-wheel-zoom.md) |
| M413 | Geometry Modeling Ops | todo | M359, M420 |  | E9 | F21 | | [M413-geometry-modeling-ops.md](PLAN/MILESTONES/M413-geometry-modeling-ops.md) |
| M415 | Geometry Attribute Ops | todo | M359, M360, M402 |  | E9 | F21 | | [M415-geometry-attribute-ops.md](PLAN/MILESTONES/M415-geometry-attribute-ops.md) |
| M414 | Geometry Advanced Modeling | todo | M359, M415 |  | E9 | F21 | | [M414-geometry-advanced-modeling.md](PLAN/MILESTONES/M414-geometry-advanced-modeling.md) |
| M416 | Geometry Deformers And Time | todo | M359, M415 |  | E9 | F21 | | [M416-geometry-deformers-and-time.md](PLAN/MILESTONES/M416-geometry-deformers-and-time.md) |
| M417 | VDB Volume Primitive | todo | M358 |  | E9 | F22 | | [M417-vdb-volume-primitive.md](PLAN/MILESTONES/M417-vdb-volume-primitive.md) |
| M418 | VDB Conversion Ops | todo | M417 |  | E9 | F22 | | [M418-vdb-conversion-ops.md](PLAN/MILESTONES/M418-vdb-conversion-ops.md) |
| M419 | VDB SDF Ops And Wrangle | todo | M418, M360 |  | E9 | F22 | | [M419-vdb-sdf-ops-and-wrangle.md](PLAN/MILESTONES/M419-vdb-sdf-ops-and-wrangle.md) |
| M376 | Geometry Display | doing | M328, M358 | bosley-native-nodes | E9 | F15 | | [M376-geometry-display.md](PLAN/MILESTONES/M376-geometry-display.md) |
| M377 | Readable CI Names | done | - | bosley-native-nodes | E2 | | | [M377-readable-ci-names.md](PLAN/MILESTONES/M377-readable-ci-names.md) |
| M378 | Scene Render Test Stability | done | - | bosley-native-nodes | E9 | | | [M378-scene-render-test-stability.md](PLAN/MILESTONES/M378-scene-render-test-stability.md) |
| M379 | GpuContext Test Segfault | done | - | bosley-native-nodes | E5 | | | [M379-gpucontext-test-segfault.md](PLAN/MILESTONES/M379-gpucontext-test-segfault.md) |
| M380 | Blocking Scheduler Aborts | done | - | bosley-native-nodes | E14 | | | [M380-blocking-scheduler-aborts.md](PLAN/MILESTONES/M380-blocking-scheduler-aborts.md) |
| M381 | Deferred Matching Coverage | doing | M331 | bosley-native-nodes | E9 | F4 | | [M381-deferred-matching-coverage.md](PLAN/MILESTONES/M381-deferred-matching-coverage.md) |
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
| M397 | Label Resize Keeps Node Top-Centre | todo | - |  | E10 | | | [M397-label-resize-keeps-node-top-centre.md](PLAN/MILESTONES/M397-label-resize-keeps-node-top-centre.md) |
| M398 | Cycles Offline Render After Scene Edits | done | - | bosley-native-nodes | E9 | |  | [M398-cycles-offline-render-after-scene-edits.md](PLAN/MILESTONES/M398-cycles-offline-render-after-scene-edits.md) |
| M399 | Node Creation Keys And Speed | done | M118 | bosley-native-nodes | E10 | | | [M399-node-creation-keys-and-speed.md](PLAN/MILESTONES/M399-node-creation-keys-and-speed.md) |
| M21 | Deep Tier-2 And Bridges | todo |  |  | E8 | | | [M21-deep-tier2-and-bridges.md](PLAN/MILESTONES/M21-deep-tier2-and-bridges.md) |
| M51 | Deep Filtering OpenDCX | todo |  |  | E8 | | | [M51-deep-filtering-opendcx.md](PLAN/MILESTONES/M51-deep-filtering-opendcx.md) |
| M32 | Deep Sample Node | todo |  |  | E8 | | | [M32-deep-sample-node.md](PLAN/MILESTONES/M32-deep-sample-node.md) |
| M93 | Serialization Boundary | done | - | slate-ferret | E7 | | | [M93-serialization-boundary.md](PLAN/MILESTONES/M93-serialization-boundary.md) |
| M100 | Content Node Hashing | done | M93 | bosley-native-nodes | E7 | | | [M100-content-node-hashing.md](PLAN/MILESTONES/M100-content-node-hashing.md) |
| M16 | Project Format Redesign | done | M93 | bosley-native-nodes | E7 | | | [M16-project-file-format-redesign.md](PLAN/MILESTONES/M16-project-file-format-redesign.md) |
| M114 | Global Undo Correctness | done | M93 | bosley-native-nodes | E7 | | | [M114-global-undo-correctness.md](PLAN/MILESTONES/M114-global-undo-correctness.md) |
| M102 | Graph Edit Operations | done | M16 | bosley-native-nodes | E7 | | | [M102-graph-edit-operations.md](PLAN/MILESTONES/M102-graph-edit-operations.md) |
| M103 | Graph Diff And Merge | todo | M102 |  | E12 | | | [M103-graph-diff-and-merge.md](PLAN/MILESTONES/M103-graph-diff-and-merge.md) |
| M104 | Merge Resolution UI | todo | M103 |  | E12 | | | [M104-merge-resolution-ui.md](PLAN/MILESTONES/M104-merge-resolution-ui.md) |
| M105 | Live Session Core | todo | M102 |  | E11 | F5 | | [M105-live-session-core.md](PLAN/MILESTONES/M105-live-session-core.md) |
| M106 | Live Session Presence And Undo | todo | M105 |  | E11 | F5 | | [M106-live-session-presence-and-undo.md](PLAN/MILESTONES/M106-live-session-presence-and-undo.md) |
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
| M136 | Edge Policy Investigation | doing | M167 | bosley-native-nodes | E3 | |  | [M136-edge-policy-investigation.md](PLAN/MILESTONES/M136-edge-policy-investigation.md) |
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
| M153 | Distortion Concatenation | todo | M147, M113 |  | E14 | | | [M153-distortion-concatenation.md](PLAN/MILESTONES/M153-distortion-concatenation.md) |
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
| M179 | Render Graph Continuation | todo | M178 |  | E6 | | | [M179-render-graph-continuation.md](PLAN/MILESTONES/M179-render-graph-continuation.md) |
| M180 | Loop Block Core | done | M178 | bosley-native-nodes | E15 | | | [M180-loop-block-core.md](PLAN/MILESTONES/M180-loop-block-core.md) |
| M181 | Loop Drivers And UI | done | M180, M71 | bosley-native-nodes | E15 | | | [M181-loop-drivers-and-ui.md](PLAN/MILESTONES/M181-loop-drivers-and-ui.md) |
| M182 | Loop Feedback | done | M180 | bosley-native-nodes | E15 | | | [M182-loop-feedback.md](PLAN/MILESTONES/M182-loop-feedback.md) |
| M183 | Loop Map Blend | todo | M180, M179, M184 |  | E15 | F14 | | [M183-loop-map-blend.md](PLAN/MILESTONES/M183-loop-map-blend.md) |
| M184 | Variable-Size Filter Kit | done | M120, M137 | gpu | E15 | | | [M184-variable-size-filter-kit.md](PLAN/MILESTONES/M184-variable-size-filter-kit.md) |
| M185 | Variable Warps | done | M113, M184 |  | E15 | | | [M185-variable-warps.md](PLAN/MILESTONES/M185-variable-warps.md) |
| M186 | Exact IDefocus | done | M131, M184 | bosley-native-nodes | E15 | | | [M186-exact-idefocus.md](PLAN/MILESTONES/M186-exact-idefocus.md) |
| M187 | Variable Sharpen And Glow | blocked | M123, M184 |  | E15 | | | [M187-variable-sharpen-and-glow.md](PLAN/MILESTONES/M187-variable-sharpen-and-glow.md) |
| M188 | Variable Median And DirBlur | blocked | M130, M184 |  | E15 | | | [M188-variable-median-and-dirblur.md](PLAN/MILESTONES/M188-variable-median-and-dirblur.md) |
| M189 | Make I-Node Action | todo | M183, M184, M181 |  | E15 | F14 | | [M189-make-i-node-action.md](PLAN/MILESTONES/M189-make-i-node-action.md) |
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
| M199 | Remove Legacy Scheduler | todo | - |  | E14 | | | [M199-remove-legacy-scheduler.md](PLAN/MILESTONES/M199-remove-legacy-scheduler.md) |
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
| M221 | Neural Optical Flow | doing | M217, M203, M204 | bosley-native-nodes | E16 | F8 | response | [M221-neural-optical-flow.md](PLAN/MILESTONES/M221-neural-optical-flow.md) |
| M222 | User Control Of GPU Placement | todo | M132 |  | E5 | | | [M222-user-control-of-gpu-placement.md](PLAN/MILESTONES/M222-user-control-of-gpu-placement.md) |
| M223 | Read And Write Knob Cleanup | done | M167 | bosley-native-nodes | E3 | | | [M223-read-and-write-knob-cleanup.md](PLAN/MILESTONES/M223-read-and-write-knob-cleanup.md) |
| M224 | Fluid Roto And RotoPaint Interaction | done | M146, M117 | misty-marten | E6 | | | [M224-fluid-roto-and-rotopaint-interaction.md](PLAN/MILESTONES/M224-fluid-roto-and-rotopaint-interaction.md) |
| M225 | ~~Clean Shutdown And Memory Release~~ | cancelled | - |  | E14 | | | [M225-clean-shutdown-and-memory-release.md](PLAN/MILESTONES/M225-clean-shutdown-and-memory-release.md) |
| M260 | Cache Invalidation Follow-ups | done | M242 | bosley-native-nodes | E14 | | | [M260-cache-invalidation-follow-ups.md](PLAN/MILESTONES/M260-cache-invalidation-follow-ups.md) |
| M261 | Roto Keyframes In Dope Sheet And Curve Editor | done | M163 | bosley-native-nodes | E6 | | | [M261-roto-keyframes-in-dope-sheet-and-curve-editor.md](PLAN/MILESTONES/M261-roto-keyframes-in-dope-sheet-and-curve-editor.md) |
| M268 | Edge-Aware Region Independence | doing | M209, M210, M211, M212, M213, M215 | bosley-native-nodes | E17 | |  | [M268-edge-aware-region-independence.md](PLAN/MILESTONES/M268-edge-aware-region-independence.md) |
| M269 | Remote GUI Checks | done |  | bosley-native-nodes |  | | | [M269-remote-gui-checks.md](PLAN/MILESTONES/M269-remote-gui-checks.md) |
| M262 | Expression Benchmark For Large Graphs | todo | - |  | E14 | F12 | | [M262-expression-benchmark-large-graphs.md](PLAN/MILESTONES/M262-expression-benchmark-large-graphs.md) |
| M263 | Expression Interpreter Overhead | todo | M262, M266 |  | E14 | F12 | | [M263-expression-interpreter-overhead.md](PLAN/MILESTONES/M263-expression-interpreter-overhead.md) |
| M264 | Expression Result Cache | todo | M263 |  | E14 | F12 | | [M264-expression-result-cache.md](PLAN/MILESTONES/M264-expression-result-cache.md) |
| M265 | Native Expression Evaluator | todo | M264 |  | E14 | F12 | | [M265-native-expression-evaluator.md](PLAN/MILESTONES/M265-native-expression-evaluator.md) |
| M266 | Expression Namespace Spike | todo | M262 |  | E14 | F12 | | [M266-expression-namespace-spike.md](PLAN/MILESTONES/M266-expression-namespace-spike.md) |
| M267 | RFVSwitch Match Choice Button | done | M72 | bosley-native-nodes | E13 | | | [M267-rfvswitch-match-choice-button.md](PLAN/MILESTONES/M267-rfvswitch-match-choice-button.md) |
| M270 | Lanczos Sinc Resampling Filters | done | - | misty-marten | E3 | | | [M270-lanczos-sinc-resampling-filters.md](PLAN/MILESTONES/M270-lanczos-sinc-resampling-filters.md) |
| M271 | Persistent Message Test Failures | done | - | bosley-native-nodes | E14 | | | [M271-persistent-message-test-failures.md](PLAN/MILESTONES/M271-persistent-message-test-failures.md) |
| M272 | Script Writes To Selected Roto Shapes | todo | - |  | E14 | | | [M272-script-writes-to-selected-roto-shapes.md](PLAN/MILESTONES/M272-script-writes-to-selected-roto-shapes.md) |
| M273 | RenderIsolation Exit Hang | done | - | bosley-native-nodes | E6 | | | [M273-renderisolation-exit-hang.md](PLAN/MILESTONES/M273-renderisolation-exit-hang.md) |
| M274 | SplineWarp Remaining Controls | done | M147, M148, M149, M150, M151 | bosley-native-nodes | E3 | | | [M274-splinewarp-remaining-controls.md](PLAN/MILESTONES/M274-splinewarp-remaining-controls.md) |
| M275 | Viewer Widget Editing | done | - | bosley-native-nodes | E10 | | | [M275-viewer-widget-editing.md](PLAN/MILESTONES/M275-viewer-widget-editing.md) |
| M276 | Upstream Reuse On Edits | done | M260 | misty-marten | E6 | F6 | | [M276-cache-responsiveness-audit.md](PLAN/MILESTONES/M276-cache-responsiveness-audit.md) |
| M300 | Unified Tiered Cache | done | M276 | bosley-native-nodes | E6 | F6 | | [M300-unified-tiered-cache.md](PLAN/MILESTONES/M300-unified-tiered-cache.md) |
| M301 | GPU Cache Tier | done | M300 | bosley-native-nodes | E6 | F6 | | [M301-gpu-cache-tier.md](PLAN/MILESTONES/M301-gpu-cache-tier.md) |
| M302 | Viewer Render State Cues | done | - | bosley-native-nodes | E6 | F6 | | [M302-viewer-render-state-cues.md](PLAN/MILESTONES/M302-viewer-render-state-cues.md) |
| M303 | Interactive Edit Latency | doing | M301 | bosley-native-nodes | E6 | F6 | | [M303-interactive-edit-latency.md](PLAN/MILESTONES/M303-interactive-edit-latency.md) |
| M277 | Graph Creation And Insert | done | M192, M193 | bosley-native-nodes | E10 | | | [M277-graph-creation-and-insert.md](PLAN/MILESTONES/M277-graph-creation-and-insert.md) |
| M278 | Translucent Selection Highlight | todo | M234 |  | E9 | | | [M278-translucent-selection-highlight.md](PLAN/MILESTONES/M278-translucent-selection-highlight.md) |
| M279 | Utility Renderer Foundation | done | M248 | bosley-native-nodes | E19 | F7 | | [M279-utility-renderer-foundation.md](PLAN/MILESTONES/M279-utility-renderer-foundation.md) |
| M280 | Utility Renderer GPU Path | done | M279, M253 | bosley-native-nodes | E19 | F7 | | [M280-utility-renderer-gpu-path.md](PLAN/MILESTONES/M280-utility-renderer-gpu-path.md) |
| M281 | Utility Materials And Lights | doing | M280, M233, M234 | bosley-native-nodes | E19 | F7 | | [M281-utility-materials-and-lights.md](PLAN/MILESTONES/M281-utility-materials-and-lights.md) |
| M283 | Utility Sampling And Motion Blur | doing | M280, M253 | bosley-native-nodes | E19 | F7 | | [M283-utility-sampling-and-motion-blur.md](PLAN/MILESTONES/M283-utility-sampling-and-motion-blur.md) |
| M282 | Utility Reflection And Refraction | todo | M281, M283 |  | E19 | F7 | | [M282-utility-reflection-and-refraction.md](PLAN/MILESTONES/M282-utility-reflection-and-refraction.md) |
| M284 | Utility Renderer Subdivision | doing | M280 | bosley-native-nodes | E19 | F7 | | [M284-utility-renderer-subdivision.md](PLAN/MILESTONES/M284-utility-renderer-subdivision.md) |
| M285 | Utility Renderer AOVs | todo | M256, M282, M283 |  | E19 | | | [M285-utility-renderer-aovs.md](PLAN/MILESTONES/M285-utility-renderer-aovs.md) |
| M286 | Utility Renderer Deep Output | todo | M285 |  | E19 | | | [M286-utility-renderer-deep-output.md](PLAN/MILESTONES/M286-utility-renderer-deep-output.md) |
| M288 | Utility Renderer Projection Modes | todo | M281, M283 |  | E19 | F7 | | [M288-utility-renderer-projection-modes.md](PLAN/MILESTONES/M288-utility-renderer-projection-modes.md) |
| M290 | Native Cryptomatte Keyer | todo | M285 |  | E19 | | | [M290-native-cryptomatte-keyer.md](PLAN/MILESTONES/M290-native-cryptomatte-keyer.md) |
| M289 | Utility Adaptive Subdivision | todo | M284 |  | E19 | | | [M289-utility-adaptive-subdivision.md](PLAN/MILESTONES/M289-utility-adaptive-subdivision.md) |
| M287 | Utility Renderer Displacement | todo | M289 |  | E19 | | | [M287-utility-renderer-displacement.md](PLAN/MILESTONES/M287-utility-renderer-displacement.md) |
| M291 | Defocus Reference Harness | todo | M131, M133 |  | E3 | F13 | | [M291-defocus-reference-harness.md](PLAN/MILESTONES/M291-defocus-reference-harness.md) |
| M292 | Traced Lens Bokeh | todo | M131, M133 |  | E3 | F13 | | [M292-traced-lens-bokeh.md](PLAN/MILESTONES/M292-traced-lens-bokeh.md) |
| M293 | ZDefocus Debug Outputs | done | M131, M133 | | E3 | F13 | | [M293-zdefocus-debug-outputs.md](PLAN/MILESTONES/M293-zdefocus-debug-outputs.md) |
| M294 | ZDefocus Engine Revisit | todo | M291, M292, M293 |  | E3 | F13 | | [M294-zdefocus-engine-revisit.md](PLAN/MILESTONES/M294-zdefocus-engine-revisit.md) |
| M295 | Native DeepDefocus | todo | M294 |  | E8 | | | [M295-native-deepdefocus.md](PLAN/MILESTONES/M295-native-deepdefocus.md) |
| M296 | Raw Prepare Output Format | done | M80 | misty-marten | E3 | | | [M296-raw-prepare-output-format.md](PLAN/MILESTONES/M296-raw-prepare-output-format.md) |
| M297 | Viewer Node Knobs Follow-ups | done | M164 | | E3 | | | [M297-viewer-node-knobs-followups.md](PLAN/MILESTONES/M297-viewer-node-knobs-followups.md) |
| M298 | RenderIsolation Taskgraph Hang | done | M177 | bosley-native-nodes | E3 | | | [M298-renderisolation-taskgraph-hang.md](PLAN/MILESTONES/M298-renderisolation-taskgraph-hang.md) |
| M299 | Roto Render Performance | todo | M224, M276, M300 |  | E6 | | | [M299-roto-render-performance.md](PLAN/MILESTONES/M299-roto-render-performance.md) |
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
| M344 | Camera Projection In Delegates | doing | M234, M325, M343 | bosley-native-nodes | E9 | F9 | | [M344-camera-projection-in-delegates.md](PLAN/MILESTONES/M344-camera-projection-in-delegates.md) |
| M345 | VDB Rendering Fixes | todo | M251, M252, M325 |  | E9 | F1 | | [M345-vdb-rendering-fixes.md](PLAN/MILESTONES/M345-vdb-rendering-fixes.md) |
| M346 | USDVolume Node | todo | M227, M345 |  | E9 | F1 | | [M346-usdvolume-node.md](PLAN/MILESTONES/M346-usdvolume-node.md) |
| M347 | Volume Material Node | todo | M236, M345, M346 |  | E9 | F1 | | [M347-volume-material-node.md](PLAN/MILESTONES/M347-volume-material-node.md) |
| M348 | Volume Viewer Support | todo | M229, M231, M232, M253, M333, M345, M346 |  | E9 | F1 | | [M348-volume-viewer-support.md](PLAN/MILESTONES/M348-volume-viewer-support.md) |
| M349 | Shader Node Spike | todo | - |  | E9 | F2 | | [M349-shader-node-spike.md](PLAN/MILESTONES/M349-shader-node-spike.md) |
| M350 | Shader Manifest And Nodes | todo | M349, M253 |  | E9 | F2 | | [M350-shader-manifest-and-nodes.md](PLAN/MILESTONES/M350-shader-manifest-and-nodes.md) |
| M351 | Shader Graph Presentation | todo | M350 |  | E9 | F2 | | [M351-shader-graph-presentation.md](PLAN/MILESTONES/M351-shader-graph-presentation.md) |
| M352 | USDMaterialBuilder Node | todo | M350, M351 |  | E9 | F2 | | [M352-usdmaterialbuilder-node.md](PLAN/MILESTONES/M352-usdmaterialbuilder-node.md) |
| M353 | Cycles Native Shaders | todo | M352, M343 |  | E9 | F2 | | [M353-cycles-native-shaders.md](PLAN/MILESTONES/M353-cycles-native-shaders.md) |
| M354 | MoonRay Native Shaders | todo | M352, M325, M343 |  | E9 | F2 | | [M354-moonray-native-shaders.md](PLAN/MILESTONES/M354-moonray-native-shaders.md) |
| M355 | Shader Tweak Node | todo | M343, M353, M354 |  | E9 | F2 | | [M355-shader-tweak-node.md](PLAN/MILESTONES/M355-shader-tweak-node.md) |
| M356 | Automatic Preview Shader | todo | M352 |  | E9 | F2 | | [M356-automatic-preview-shader.md](PLAN/MILESTONES/M356-automatic-preview-shader.md) |
| M357 | Repo Cleanup | todo | - |  | E14 |  | | [M357-repo-cleanup.md](PLAN/MILESTONES/M357-repo-cleanup.md) |
| M364 | Delegate Cancel Responsiveness | done | M254 | bosley-native-nodes | E9 | F9 | | [M364-delegate-cancel-responsiveness.md](PLAN/MILESTONES/M364-delegate-cancel-responsiveness.md) |
| M365 | Delegate Display Transform | todo | M254, M340 |  | E9 |  | | [M365-delegate-display-transform.md](PLAN/MILESTONES/M365-delegate-display-transform.md) |
| M368 | Native Instance Material Binding | done | M341, M342 | bosley-native-nodes | E9 | F9 | | [M368-native-instance-material-binding.md](PLAN/MILESTONES/M368-native-instance-material-binding.md) |
| M366 | Mixed Pixel Aspect Tiles | todo | M300 |  | E14 |  | | [M366-mixed-pixel-aspect-tiles.md](PLAN/MILESTONES/M366-mixed-pixel-aspect-tiles.md) |
| M367 | USD Camera Exposure | todo | M251, M252, M254 |  | E9 |  | | [M367-usd-camera-exposure.md](PLAN/MILESTONES/M367-usd-camera-exposure.md) |
| M369 | Curve Slopes Independent Of Edit Order | todo | M304 |  | E14 |  | | [M369-curve-slopes-independent-of-edit-order.md](PLAN/MILESTONES/M369-curve-slopes-independent-of-edit-order.md) |
| M370 | Roto Point List Render Isolation | todo | M304 |  | E6 |  | | [M370-roto-point-list-render-isolation.md](PLAN/MILESTONES/M370-roto-point-list-render-isolation.md) |
| M371 | Cycles Abort Test Thread Count | done | - | bosley-native-nodes | E9 |  | | [M371-cycles-abort-test-thread-count.md](PLAN/MILESTONES/M371-cycles-abort-test-thread-count.md) |
| M372 | User Knob Engine Edits | todo | - |  | E10 | F16 | | [M372-user-knob-engine-edits.md](PLAN/MILESTONES/M372-user-knob-engine-edits.md) |
| M373 | In-Panel Knob Editor | todo | M372 |  | E10 | F16 | | [M373-in-panel-knob-editor.md](PLAN/MILESTONES/M373-in-panel-knob-editor.md) |
| M374 | Remove Node Magnify On Ctrl-Scroll | todo | - |  | E10 |  | | [M374-remove-node-magnify-on-ctrl-scroll.md](PLAN/MILESTONES/M374-remove-node-magnify-on-ctrl-scroll.md) |
| M375 | No CPU Vulkan Auto-Select | done | - | bosley-native-nodes | E5 |  | | [M375-no-cpu-vulkan-auto-select.md](PLAN/MILESTONES/M375-no-cpu-vulkan-auto-select.md) |
| M400 | Pointer Direction Badges | todo | - |  | E10 |  | | [M400-pointer-direction-badges.md](PLAN/MILESTONES/M400-pointer-direction-badges.md) |
| M425 | No ONNX Runtime Telemetry | todo | - |  | E16 |  | | [M425-no-onnx-runtime-telemetry.md](PLAN/MILESTONES/M425-no-onnx-runtime-telemetry.md) |

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

# Features

| ID | Name | Status | Owner | Needs | Branch | Goal |
|----|------|--------|-------|-------|--------|------|
| F1 | VDB Volumes | open |  |  | feature/f1-vdb-volumes | Bring a VDB into the scene, shade it with one material across Storm, Cycles and MoonRay, and inspect it in the viewer. |
| F2 | Native Shader Networks | open |  |  | feature/f2-native-shader-networks | Build, tweak and render each renderer's native shading networks from one builder node, with full or nearly full coverage of Cycles and MoonRay. |
| F3 | Cycles And MoonRay Delegates | landed | misty-marten |  | feature/f3-cycles-and-moonray-delegates | Bundle Cycles and MoonRay and render through them in USDRender and the viewer, with full render settings, tested on one AppImage. |
| F4 | Heavy Scene Viewer | open |  |  | feature/f4-heavy-scene-viewer | Open and inspect production-sized USD scenes without blocking, with deferred loading, draw states and full prim inspection. |
| F5 | Live Sessions V1 | open |  |  | feature/f5-live-sessions-v1 | Let several artists edit one graph live, with presence, per-user undo and viewer following. |
| F6 | Unified Tiered Cache | open |  |  | feature/f6-unified-tiered-cache | Reuse unchanged upstream work across RAM, disk and VRAM so a downstream edit reaches the viewer within budget, and show truthfully what is rendering. |
| F7 | Natron Utility Core | open |  |  | feature/f7-natron-utility-core | A usable Nucleon Utility renderer on CPU and GPU, with materials, lights, sampling, reflection, subdivision and projection modes. Ships only after F9 - Delegate Projection lands, for M364 - Delegate Cancel Responsiveness. |
| F8 | Motion Vectors | open |  |  | feature/f8-motion-vectors | Generate dense motion vectors classically, on the GPU and with a neural model, and move Roto and SplineWarp curves along them. |
| F9 | Delegate Projection | open |  |  | feature/f9-delegate-projection | Live textures and camera projection in Cycles and MoonRay, with per-placement and deforming-geometry projection. |
| F10 | Viewer Polish | landed | bosley-native-nodes |  | feature/f10-viewer-polish | Ship a batch of small viewer and properties-panel fixes on one build and one UAT. |
| F11 | Texture Painting | open |  |  | feature/f11-texture-painting | Paint directly on meshes in the 3D view with UDIM textures, layering over existing materials, baking and paint-through inputs. |
| F12 | Expression Speed | open |  |  | feature/f12-expression-speed | Benchmark Python knob expressions and make them cheap on large graphs, through a lighter interpreter path, a result cache and a native evaluator. |
| F13 | ZDefocus Rework | open |  |  | feature/f13-zdefocus-rework | Measure ZDefocus against ground truth, add traced lenses and debug outputs, and revisit its engine from the results. |
| F14 | Map Blend I-Nodes | open |  |  | feature/f14-map-blend-i-nodes | Turn any subgraph into a Map-driven I-node through LoopEnd's Map blend and one Make I-Node action. |
| F15 | Native Geometry | open |  |  | feature/f15-native-geometry | A SOP-style native geometry context: a Houdini-like detail, bridges to USD, operators, Python and VEX-like wrangles, point editing and a geometry spreadsheet. |
| F16 | User Knob Editor | open |  |  | feature/f16-user-knob-editor | Create and manage user knobs in place on the properties panel, with type changes, undo, button scripts, drag-in links and copy/paste. |
| F17 | OpenNucleon Flag Day | open |  |  | feature/f17-opennucleon-flag-day | Rename Natron to OpenNucleon/Nucleon throughout, code included, with new art and the GPLv3 relicence, landed in one switchover at a quiet time the user chooses. |
| F18 | Geometry Loops And Solver | open |  |  | feature/f18-geometry-loops-and-solver | Loop and solve native geometry: For-Each blocks on render contexts, a native geometry file cache, and a Solver SOP with a sim cache. |
| F19 | Particles | open |  |  | feature/f19-particles | A POP-style particle system on native geometry, with sources, forces, interaction, collisions, a GPU path and 2D image particles. Waits for F22 - Native VDB to land, for collisions. |
| F20 | 2D Fluids | open |  |  | feature/f20-2d-fluids | Grid smoke and fire simulated in image space from image sources and motion vectors, with a shading node. |
| F21 | Geometry Operators | open |  |  | feature/f21-geometry-operators | Houdini SOP operator batches: modeling, advanced modeling, attribute, deformer and time ops, PolyExtrude/Boolean/Fuse, and a Bezier curve primitive. |
| F22 | Native VDB | open |  |  | feature/f22-native-vdb | An OpenVDB volume primitive in native geometry, bridged to USD, with conversion, SDF ops and a volume wrangle. |

# Open questions
- **For /cat-plan (user decision 2026-11-13):** add a new milestone "UDIM Textures In Delegates" (E9, no Feature, Deps M311 and F9 - Delegate Projection landed / M343) carrying M311 - UDIM Live Textures' Phase 311.4 brief (P4.T1, UDIM live textures in Cycles and MoonRay), split out so F11 - Texture Painting does not wait on F9.
- **M221 - Neural Optical Flow (PM, 2026-11-12):** the SEA-RAFT weights (`MemorySlices/Tartan-C-T-TSKH-spring540x960-M`, `model.safetensors`, BSD-3, GPL-compatible) can't be downloaded here: HuggingFace's CDN `us.aws.cdn.hf.co` is unreachable from the sandbox. Either allow that host, or drop the file at `/tmp/m221-model/spring-M.safetensors` (dev host). Also confirm the training-data licence strings for the manifest (TartanAir CC BY 4.0 confirmed; KITTI, FlyingThings3D/Chairs, Sintel, HD1K, Spring sites unreachable). The archive will need uploading to a fork release by you. P2.T1 (node integration on the fixture model) continues meanwhile.
- **Nucleon 1.0 must-land list (user, 2026-11-10: "I'll decide later"):** which Features and milestones must be on `main` before 1.0. M396 - Release 1.0 Cut is held, not eligible for pickup, until the user names them; they are then added to its `Deps`.
- **Site repo setup for E21 - Nucleon 1.0 Release (user action, after the `opennucleon` org exists):** in `opennucleon/opennucleon.github.io`, enable Pages from the `gh-pages` branch, add a deploy key with write access, and store its private half as the OpenNucleon secret `SITE_DEPLOY_KEY`. M392 - Docs Publishing needs this. Once M388 - Docs Site Scaffold merges, add its `Docs` job to `main`'s required checks.
- **M382 - Nucleon Logo And Artwork (PM, 2026-11-10):** pick one of the four logo concepts (nucleus, orbital, N monogram, node graph), sent as one PNG per concept and background, with notes for the first refinement round.
- **F17 - OpenNucleon Flag Day is held (user, 2026-11-10):** only M382 - Nucleon Logo And Artwork may start now, and it merges into the Feature branch only. M383 - Rename Script And Guard, M384 - Plugin Forks Rename, M157 - Relicense To GPLv3 Or Later, M385 - OpenNucleon Code Rename, M386 - Nucleon Branding And Packaging and M387 - Dev Infrastructure Rename are not eligible for pickup until the user names the switchover time. Before that, the user creates the `opennucleon` GitHub org, transfers and renames the repo (`opennucleon/OpenNucleon`) and the forks (`openfx-{io,misc,arena,supportext}`, `openfx-natron` → `openfx-nucleon`), and creates `opennucleon.github.io`. After the landing: every open branch merges main and runs `tools/rename/rename.py --changed-since origin/main`, and `/cat-plan` sweeps the `todo` milestone briefs through the same script.
- **M276 - Upstream Reuse On Edits (PM, 2026-11-06):** the latency baseline and GPU counts need your host: `build/appimages/Natron-m276-upstream-reuse-on-edits.AppImage` with the steps in its `.md`. Also: add a task to M300 - Unified Tiered Cache that prunes dependencies behind a cache hit? Recommend yes; its gate can't pass without it.
- **NativeSplineWarpGpuTest timeouts (PM, 2026-11-06):** the suite takes about 150-285 s against a 300 s limit on loaded runners and timed out in one leg on the M296 - Raw Prepare Output Format, M333 - Prim Inspector and M320 - Write All Input Metadata runs, with no spline or warp code touched. Treated as runner load. M270 - Lanczos Sinc Resampling Filters adds filters to this suite, so it raises the limit or splits the suite in P2.T4.
- **F3 - Cycles And MoonRay Delegates, render settings that do nothing (PM, 2026-11-06):** MoonRay's denoiser likely doesn't run in our build (traced, not run), and Cycles' Path Guiding isn't compiled in. Keep them as controls whose tooltips say so? Recommend: patch MoonRay to select OIDN and mark the guides on outputs (a follow-up milestone), and build Cycles with openpgl only if you want guiding. Raise with the panel screenshots.
- **NatronRenderer and [Project] (PM, 2026-11-02):** `NatronRenderer -w Write 3-3 defocus_retime.ntpz` fails with "Could not open [Project]/../…": the Write path's [Project] is not expanded when the renderer opens an .ntpz directly (seen on the M125 branch, before M245 - Write Path Variables). Re-check once M245 is on main; if it persists, file it.
- **Post-merge checks for the user (user, 2026-11-02):** these merged on approvals given against AppImages that predated their round-1 fixes; test the listed behaviour on main once merged. M228 - 3D Viewer Navigation (camera knobs as GUI edits, Storm GL lock), M229 - 3D Viewer Guides And Display (guide-size preference, sized light guides, number-knob layout), M230 - 3D Transform Widgets (VkResult in the GPU error), M245 - Write Path Variables (path variables on project RFVs, dialog, rename rewrite, undo). Batch merge of 2026-11-05 (lanes combined after their UAT builds; test on main): Keyer and ChromaKeyer with the M95 - Knob Cleanup And Keyer Redesign behaviour under the plain IDs from M111 - Native Node Identity Cleanup; Radial and the other shapes from M128 - Native Shape Generators have no Interactive Update or HiDPI knobs; ErodeDilate and M186 - Exact IDefocus together; M160 - Lensfun Lens Correction and M233/M234 live textures and projection under plain plugin IDs; the three `tools/bench/comps/*.ntpz` comps were edited by script and not loaded in Natron: load them, or regenerate with `COMP=keying_grade COMPS_OUT=tools/bench/comps NatronRenderer -b tools/bench/comps/build_comps.py`, once per COMP. M118 - Large Graph GUI Responsiveness (merged 2026-11-12, PR #259): on the laptop, wheel zoom in the node graph pivots on the cursor. M116 - GPU Region Overlay (merged 2026-11-12, PR #261; approved before its review fixes): with the overlay on, drag a region member (only its outline follows), zoom out to low detail and toggle the overlay (faded nodes fade and unfade), select all (no flicker); the tooltip now says it fades nodes that computed no pixels.
- **M206 - Neural Raw Denoise:** remind the user later about the hand-made darktable 5 reference export (user, 2026-11-02: "remind me later").

- M238 - EdgeExtend Options (bosley-native-nodes): which Nuke options to add (recommend all five), replace Size/Slices with push-pull (recommend yes), edge mask in an output channel (recommend yes). See the milestone's Open questions.
- M79 - Layered Document Readers (bosley-native-nodes): library choice (recommend in-tree OIIO ImageInputs on ZipStore + QXmlStreamReader), layer model and XCF composite, fixture authoring. See the milestone's Open questions.


- **M123 - Native Sharpen And Glow, Bloom design:** the user requires GPU residency (no CPU fallback) if at all possible; candidates are being spiked. Filter default (FIR Gaussian, recommended, or CImg's quasi-Gaussian) still open.
- **M198 - RenderEngine On Node, M199 - Remove Legacy Scheduler, M200 - Folded Knob Shared Storage, M201 - Trimap Removal:** do not start without the user's go-ahead (user, 2026-10-25, M31 triage). Each stub's `Blocked on:` names what the user is waiting for. M202 - Clean-Hash Cache Stores needs no go-ahead.
- **M46 - Labelmaker Annotations:** on hold, not eligible for pickup (user, 2026-10-27, during its grilling). Resume only with the user's go-ahead, then finish the interview (`/cat-grill M46`).
- **M187 - Variable Sharpen And Glow:** on hold, not eligible for pickup (user, 2026-10-27, inbox). Resume only with the user's say-so.
- **M188 - Variable Median And DirBlur:** on hold, not eligible for pickup (user, 2026-10-27, inbox). Resume only with the user's say-so.
- **M174 - Wayland Support:** blocked until the user approves starting it (user, 2026-10-24). Stub; elaborate on approval.


- **M64 - Tiled Rendering:** dropped for now (user, 2026-10-08): this 4-core dev host is too slow for the Phase 64.7 re-evaluation benchmarks to be meaningful. Resume only on a faster host and with the user's go-ahead.
- **M166 - Non-Float Plane Model:** the user wants to rethink the model before it is planned: Read emitting EXR half/uint32 layers natively, and possibly a bit depth per layer. Settle the framing in `/cat-discuss`, then elaborate.
