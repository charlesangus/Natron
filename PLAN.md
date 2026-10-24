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
- **M31 (architectural cleanup) is authored but deferred** (2026-09-15): a
  parking place for structural debts that feature work exposes — first entry
  is moving the `RenderEngine` from the `OutputEffectInstance` base class to
  `Node` by composition, surfaced by M18.P3.T8a. Do not start it without an
  explicit user go-ahead; add tasks to it as they surface instead of folding
  refactors into feature milestones.

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

| ID | Milestone | Status | Deps | Owner | Epic | File |
|----|-----------|--------|------|-------|------|------|
| M0 | Fork And Cut Scope | done |  |  | E1 | [M0-fork-cut-scope.md](PLAN/MILESTONES/M0-fork-cut-scope.md) |
| M1 | Toolchain Baseline | done |  |  | E1 | [M1-toolchain-baseline.md](PLAN/MILESTONES/M1-toolchain-baseline.md) |
| M2 | Qt6 Migration | done |  |  | E1 | [M2-qt6-migration.md](PLAN/MILESTONES/M2-qt6-migration.md) |
| M7 | Local Incremental Builds | done |  |  | E2 | [M7-local-incremental-builds.md](PLAN/MILESTONES/M7-local-incremental-builds.md) |
| M8 | Branching And CI/CD | done |  |  | E2 | [M8-branching-and-cicd.md](PLAN/MILESTONES/M8-branching-and-cicd.md) |
| M9 | ~~Drop Vendored OFX~~ | cancelled |  |  |  | [M9-drop-vendored-ofx.md](PLAN/MILESTONES/M9-drop-vendored-ofx.md) |
| M10 | Clean-Sheet CI/CD | done |  |  | E2 | [M10-cicd-clean-sheet.md](PLAN/MILESTONES/M10-cicd-clean-sheet.md) |
| M3 | Dependency Modernization | done |  |  | E1 | [M3-dependency-modernization.md](PLAN/MILESTONES/M3-dependency-modernization.md) |
| M4 | CI/CD Rebuild | done |  |  | E2 | [M4-cicd-rebuild.md](PLAN/MILESTONES/M4-cicd-rebuild.md) |
| M12 | Documentation Cruft Removal | done |  |  | E2 | [M12-documentation-cruft-removal.md](PLAN/MILESTONES/M12-documentation-cruft-removal.md) |
| M13 | Full OFX Plugin Set | done |  |  | E1 | [M13-full-ofx-plugin-set.md](PLAN/MILESTONES/M13-full-ofx-plugin-set.md) |
| M5 | Test And Correctness Baseline | done |  |  | E2 | [M5-test-correctness-baseline.md](PLAN/MILESTONES/M5-test-correctness-baseline.md) |
| M6 | Documentation Pass | done |  |  | E2 | [M6-documentation-pass.md](PLAN/MILESTONES/M6-documentation-pass.md) |
| M15 | Release Packaging | done |  |  | E2 | [M15-release-packaging.md](PLAN/MILESTONES/M15-release-packaging.md) |
| M11 | OFX Plugin Integration Test | done |  |  | E2 | [M11-ofx-plugin-integration-test.md](PLAN/MILESTONES/M11-ofx-plugin-integration-test.md) |
| M14 | Documentation Orphan Branch | done |  |  | E2 | [M14-documentation-tree-and-doc-ci.md](PLAN/MILESTONES/M14-documentation-tree-and-doc-ci.md) |
| M17 | Typed Edges Framework | done |  |  | E8 | [M17-typed-edges-native-framework.md](PLAN/MILESTONES/M17-typed-edges-native-framework.md) |
| M26 | Test Teardown Flake Fix | done |  |  | E2 | [M26-test-fixture-teardown-flake.md](PLAN/MILESTONES/M26-test-fixture-teardown-flake.md) |
| M18 | Deep Compositing v1 | done |  |  | E8 | [M18-deep-compositing-v1.md](PLAN/MILESTONES/M18-deep-compositing-v1.md) |
| M23 | Relocatable Release Bundles | done |  |  | E2 | [M23-relocatable-release-bundles.md](PLAN/MILESTONES/M23-relocatable-release-bundles.md) |
| M39 | Layers Not Planes | done |  |  | E4 | [M39-layers-not-planes.md](PLAN/MILESTONES/M39-layers-not-planes.md) |
| M57 | Write Layer Regressions | done |  |  | E4 | [M57-write-node-plane-layer-regressions.md](PLAN/MILESTONES/M57-write-node-plane-layer-regressions.md) |
| M58 | Write Multiplane Pixel Bug | done |  |  | E4 | [M58-write-multiplane-pixel-data-bug.md](PLAN/MILESTONES/M58-write-multiplane-pixel-data-bug.md) |
| M59 | Pre-Commit Auto-Format | done |  |  | E2 | [M59-pre-commit-auto-format.md](PLAN/MILESTONES/M59-pre-commit-auto-format.md) |
| M38 | Layer Channel Widget | done |  |  | E4 | [M38-channel-layer-ui-organization.md](PLAN/MILESTONES/M38-channel-layer-ui-organization.md) |
| M43 | ~~Drop Premult Concept~~ | cancelled |  |  | E4 | [M43-drop-premult-concept.md](PLAN/MILESTONES/M43-drop-premult-concept.md) |
| M34 | Native Shuffle Node | done |  |  | E4 | [M34-new-native-shuffle-node.md](PLAN/MILESTONES/M34-new-native-shuffle-node.md) |
| M61 | Time-Varying Layers | done |  |  | E4 | [M61-layers-that-vary-with-time.md](PLAN/MILESTONES/M61-layers-that-vary-with-time.md) |
| M65 | RGBA Layers | done |  |  | E4 | [M65-rgba-rgb-alpha-layers.md](PLAN/MILESTONES/M65-rgba-rgb-alpha-layers.md) |
| M66 | Alpha-Only Plugin Support | done |  |  | E4 | [M66-plugin-alpha-only-moderate.md](PLAN/MILESTONES/M66-plugin-alpha-only-moderate.md) |
| M35 | ~~Remove Implicit Shuffle~~ | cancelled |  |  | E4 | [M35-remove-implicit-shuffle-elsewhere.md](PLAN/MILESTONES/M35-remove-implicit-shuffle-elsewhere.md) |
| M36 | ~~New Channel Affordance~~ | cancelled |  |  | E4 | [M36-new-channel-affordance.md](PLAN/MILESTONES/M36-new-channel-affordance.md) |
| M37 | Channel Management Nodes | done |  |  | E4 | [M37-channel-management-nodes.md](PLAN/MILESTONES/M37-channel-management-nodes.md) |
| M50 | Project OCIO Support | done |  |  | E3 | [M50-proper-ocio-support.md](PLAN/MILESTONES/M50-proper-ocio-support.md) |
| M60 | Deep Layers And Channels | done |  |  | E8 | [M60-deep-layers-and-channels.md](PLAN/MILESTONES/M60-deep-layers-and-channels.md) |
| M62 | Render Scaling Hotspots | done |  |  | E6 | [M62-render-scaling-hotspots.md](PLAN/MILESTONES/M62-render-scaling-hotspots.md) |
| M63 | Task-Graph Render Scheduler | done |  |  | E6 | [M63-task-graph-render-scheduler.md](PLAN/MILESTONES/M63-task-graph-render-scheduler.md) |
| M67 | Native Core Nodes | done |  |  | E3 | [M67-native-core-nodes.md](PLAN/MILESTONES/M67-native-core-nodes.md) |
| M28 | Free RAM Fix | done |  |  | E6 | [M28-free-ram-reads-memfree.md](PLAN/MILESTONES/M28-free-ram-reads-memfree.md) |
| M30 | Automated Beta Releases | done |  |  | E2 | [M30-automated-beta-releases.md](PLAN/MILESTONES/M30-automated-beta-releases.md) |
| M73 | Native Core Nodes Wrap-Up | done |  |  | E3 | [M73-native-core-nodes-wrap-up.md](PLAN/MILESTONES/M73-native-core-nodes-wrap-up.md) |
| M64 | Tiled Rendering | blocked | M63 |  | E6 | [M64-tiled-rendering.md](PLAN/MILESTONES/M64-tiled-rendering.md) |
| M74 | Native Metadata Core | done |  |  | E3 | [M74-native-metadata-core.md](PLAN/MILESTONES/M74-native-metadata-core.md) |
| M75 | Native Read | done |  |  | E3 | [M75-native-read.md](PLAN/MILESTONES/M75-native-read.md) |
| M76 | Native Write | done | M75 |  | E3 | [M76-native-write.md](PLAN/MILESTONES/M76-native-write.md) |
| M77 | Native Metadata Nodes | done | M75 | bosley-native-nodes | E3 | [M77-native-metadata-nodes.md](PLAN/MILESTONES/M77-native-metadata-nodes.md) |
| M78 | Metadata In Expressions | done | M75, M77 | bosley-native-nodes | E3 | [M78-metadata-in-expressions.md](PLAN/MILESTONES/M78-metadata-in-expressions.md) |
| M79 | Layered Document Readers | todo | M75 |  | E3 | [M79-layered-document-readers.md](PLAN/MILESTONES/M79-layered-document-readers.md) |
| M80 | Native RAW Support | doing | M75, M85, M120, M137 | bosley-native-nodes | E3 | [M80-native-raw-support.md](PLAN/MILESTONES/M80-native-raw-support.md) |
| M159 | Kind-Agnostic Groups | doing | - | bosley-native-nodes | E3 | [M159-kind-agnostic-groups.md](PLAN/MILESTONES/M159-kind-agnostic-groups.md) |
| M160 | Lensfun Lens Correction | todo | M80, M113 |  | E3 | [M160-lensfun-lens-correction.md](PLAN/MILESTONES/M160-lensfun-lens-correction.md) |
| M82 | Render Cost Profiling | done |  |  | E6 | [M82-render-cost-profiling.md](PLAN/MILESTONES/M82-render-cost-profiling.md) |
| M83 | GPU Compute Backend Spike | done |  |  | E5 | [M83-gpu-compute-backend-spike.md](PLAN/MILESTONES/M83-gpu-compute-backend-spike.md) |
| M84 | GPU Placement And Residency | done |  |  | E5 | [M84-gpu-placement-and-residency.md](PLAN/MILESTONES/M84-gpu-placement-and-residency.md) |
| M101 | Clean-Sheet Merge Node | todo | M84, M137, M158 |  | E3 | [M101-clean-sheet-merge-node.md](PLAN/MILESTONES/M101-clean-sheet-merge-node.md) |
| M111 | Native Node Identity Cleanup | todo | M76, M101, M120 |  | E3 | [M111-native-node-identity-cleanup.md](PLAN/MILESTONES/M111-native-node-identity-cleanup.md) |
| M112 | Native Node Behaviour Fixes | done | - | bosley-native-nodes | E3 | [M112-native-node-behaviour-fixes.md](PLAN/MILESTONES/M112-native-node-behaviour-fixes.md) |
| M81 | Raw GPU Kernels | todo | M80, M85 |  | E5 | [M81-raw-gpu-kernels.md](PLAN/MILESTONES/M81-raw-gpu-kernels.md) |
| M85 | Native GPU Kernels | done | M84 | gpu | E5 | [M85-native-gpu-kernels.md](PLAN/MILESTONES/M85-native-gpu-kernels.md) |
| M107 | GPU Viewer Hand-off | doing | M85 | gpu | E5 | [M107-gpu-viewer-hand-off.md](PLAN/MILESTONES/M107-gpu-viewer-hand-off.md) |
| M108 | Retire Engine GL Host Path | done | M85 | gpu | E5 | [M108-retire-engine-gl-host-path.md](PLAN/MILESTONES/M108-retire-engine-gl-host-path.md) |
| M86 | OFX 1.5 GPU Suites | todo | M85, M108 |  | E5 | [M86-ofx-gpu-suites.md](PLAN/MILESTONES/M86-ofx-gpu-suites.md) |
| M87 | Point-Op Kernel Fusion | todo | M85, M126 |  | E5 | [M87-point-op-kernel-fusion.md](PLAN/MILESTONES/M87-point-op-kernel-fusion.md) |
| M109 | GPU Region Engine Hardening | done | M85 | gpu | E5 | [M109-gpu-region-engine-hardening.md](PLAN/MILESTONES/M109-gpu-region-engine-hardening.md) |
| M110 | Remaining Native GPU Ports | doing | M85, M109, M126, M137 | gpu | E5 | [M110-remaining-native-gpu-ports.md](PLAN/MILESTONES/M110-remaining-native-gpu-ports.md) |
| M88 | FIR Gaussian Blur | done |  |  | E6 | [M88-fir-gaussian-blur.md](PLAN/MILESTONES/M88-fir-gaussian-blur.md) |
| M89 | Shared Worktree Ccache | done | - |  | E2 | [M89-shared-worktree-ccache.md](PLAN/MILESTONES/M89-shared-worktree-ccache.md) |
| M90 | Render Path Serial Overhead | done | M84 | bosley-native-nodes | E6 | [M90-render-path-serial-overhead.md](PLAN/MILESTONES/M90-render-path-serial-overhead.md) |
| M91 | NVIDIA GPU Validation | doing | M84 | bosley-faux | E5 | [M91-nvidia-gpu-validation.md](PLAN/MILESTONES/M91-nvidia-gpu-validation.md) |
| M116 | GPU Region Overlay | todo | M81, M85, M86, M87, M91, M107, M108, M109, M110 |  | E5 | [M116-gpu-region-overlay.md](PLAN/MILESTONES/M116-gpu-region-overlay.md) |
| M27 | Real Debug Build | done | - | | E2 | [M27-debug-build-defines-ndebug.md](PLAN/MILESTONES/M27-debug-build-defines-ndebug.md) |
| M22 | Missing Plugin Placeholder | done | M75 | bosley-native-nodes | E3 | [M22-missing-plugin-placeholder.md](PLAN/MILESTONES/M22-missing-plugin-placeholder.md) |
| M31 | Architectural Cleanup | todo | M75, M76 |  | E14 | [M31-architectural-cleanup.md](PLAN/MILESTONES/M31-architectural-cleanup.md) |
| M69 | Minor Cleanup | done | M44, M55 | amber-falcon | E10 | [M69-minor-cleanup.md](PLAN/MILESTONES/M69-minor-cleanup.md) |
| M24 | Node Graph Category Colour | done | - |  | E10 | [M24-node-graph-category-colour.md](PLAN/MILESTONES/M24-node-graph-category-colour.md) |
| M44 | Trackball Colour Editing | done | - |  | E10 | [M44-trackball-colour-editing.md](PLAN/MILESTONES/M44-trackball-colour-editing.md) |
| M55 | Node Graph Polish | done | M24 |  | E10 | [M55-polish-node-graph-interaction.md](PLAN/MILESTONES/M55-polish-node-graph-interaction.md) |
| M46 | Labelmaker Annotations | todo | M56, M118 |  | E10 | [M46-labelmaker-annotations.md](PLAN/MILESTONES/M46-labelmaker-annotations.md) |
| M56 | Visual And Menu Polish | done | M44, M55 | amber-falcon | E10 | [M56-polish-visual-and-menus.md](PLAN/MILESTONES/M56-polish-visual-and-menus.md) |
| M45 | Native Tab Menu | done | M55 | amber-falcon | E10 | [M45-tabtabtab-native-tab-menu.md](PLAN/MILESTONES/M45-tabtabtab-native-tab-menu.md) |
| M25 | GL Init FP Guard | todo |  |  | E2 | [M25-debug-fp-trap-gl-init.md](PLAN/MILESTONES/M25-debug-fp-trap-gl-init.md) |
| M29 | ~~Independent Repository~~ | cancelled |  |  | E2 | [M29-independent-repository.md](PLAN/MILESTONES/M29-independent-repository.md) |
| M68 | Headless GL EGL | todo |  |  | E2 | [M68-headless-gl-egl-backend.md](PLAN/MILESTONES/M68-headless-gl-egl-backend.md) |
| M19 | USD/Hydra Foundation | todo |  |  | E9 | [M19-usd-hydra-foundation.md](PLAN/MILESTONES/M19-usd-hydra-foundation.md) |
| M20 | 3D Node Vocabulary | todo |  |  | E9 | [M20-3d-node-vocabulary.md](PLAN/MILESTONES/M20-3d-node-vocabulary.md) |
| M52 | USD Layer-Stack Completion | todo |  |  | E9 | [M52-usd-layer-stack-completion.md](PLAN/MILESTONES/M52-usd-layer-stack-completion.md) |
| M53 | Projected Texture Painting | todo |  |  | E9 | [M53-projected-texture-painting.md](PLAN/MILESTONES/M53-projected-texture-painting.md) |
| M54 | SOPs-Style Geometry | todo |  |  | E9 | [M54-sops-style-geometry.md](PLAN/MILESTONES/M54-sops-style-geometry.md) |
| M21 | Deep Tier-2 And Bridges | todo |  |  | E8 | [M21-deep-tier2-and-bridges.md](PLAN/MILESTONES/M21-deep-tier2-and-bridges.md) |
| M51 | Deep Filtering OpenDCX | todo |  |  | E8 | [M51-deep-filtering-opendcx.md](PLAN/MILESTONES/M51-deep-filtering-opendcx.md) |
| M32 | Deep Sample Node | todo |  |  | E8 | [M32-deep-sample-node.md](PLAN/MILESTONES/M32-deep-sample-node.md) |
| M93 | Serialization Boundary | done | - | slate-ferret | E7 | [M93-serialization-boundary.md](PLAN/MILESTONES/M93-serialization-boundary.md) |
| M100 | Content Node Hashing | done | M93 | bosley-native-nodes | E7 | [M100-content-node-hashing.md](PLAN/MILESTONES/M100-content-node-hashing.md) |
| M16 | Project Format Redesign | blocked | M93 | bosley-native-nodes | E7 | [M16-project-file-format-redesign.md](PLAN/MILESTONES/M16-project-file-format-redesign.md) |
| M114 | Global Undo Correctness | todo | M93 |  | E7 | [M114-global-undo-correctness.md](PLAN/MILESTONES/M114-global-undo-correctness.md) |
| M102 | Graph Edit Operations | todo | M16 |  | E7 | [M102-graph-edit-operations.md](PLAN/MILESTONES/M102-graph-edit-operations.md) |
| M103 | Graph Diff And Merge | todo | M102 |  | E12 | [M103-graph-diff-and-merge.md](PLAN/MILESTONES/M103-graph-diff-and-merge.md) |
| M104 | Merge Resolution UI | todo | M103 |  | E12 | [M104-merge-resolution-ui.md](PLAN/MILESTONES/M104-merge-resolution-ui.md) |
| M105 | Live Session Core | todo | M102 |  | E11 | [M105-live-session-core.md](PLAN/MILESTONES/M105-live-session-core.md) |
| M106 | Live Session Presence And Undo | todo | M105 |  | E11 | [M106-live-session-presence-and-undo.md](PLAN/MILESTONES/M106-live-session-presence-and-undo.md) |
| M70 | RFV Declaration And Storage | todo |  |  | E13 | [M70-rfv-declaration-and-storage.md](PLAN/MILESTONES/M70-rfv-declaration-and-storage.md) |
| M71 | RFV Render Context | todo |  |  | E13 | [M71-rfv-render-context.md](PLAN/MILESTONES/M71-rfv-render-context.md) |
| M72 | RFV Integration And UAT | todo |  |  | E13 | [M72-rfv-integration-and-uat.md](PLAN/MILESTONES/M72-rfv-integration-and-uat.md) |
| M92 | Linked Nodes | todo | M24 |  | E10 | [M92-linked-nodes.md](PLAN/MILESTONES/M92-linked-nodes.md) |
| M95 | Knob Cleanup And Keyer Redesign | todo | M69, M76, M137 |  | E3 | [M95-knob-cleanup-and-keyer-redesign.md](PLAN/MILESTONES/M95-knob-cleanup-and-keyer-redesign.md) |
| M96 | Weighted Curve Tangents | todo | M93 |  | E7 | [M96-weighted-curve-tangents.md](PLAN/MILESTONES/M96-weighted-curve-tangents.md) |
| M97 | ~~Value-Based Node Hashing~~ | cancelled |  |  | E7 | [M97-value-based-node-hashing.md](PLAN/MILESTONES/M97-value-based-node-hashing.md) |
| M94 | Delete Unused Maintainer Skill | todo | - |  | E2 | [M94-delete-unused-maintainer-skill.md](PLAN/MILESTONES/M94-delete-unused-maintainer-skill.md) |
| M115 | Build Speed And Artifact Size | done | - | bosley-native-nodes | E2 | [M115-build-speed-and-artifact-size.md](PLAN/MILESTONES/M115-build-speed-and-artifact-size.md) |
| M113 | Native Warp Nodes | todo | M120, M56 |  | E3 | [M113-native-node-rewrites-continued.md](PLAN/MILESTONES/M113-native-node-rewrites-continued.md) |
| M120 | Native Node Port Kit | done | M85, M137 | bosley-native-nodes | E3 | [M120-native-node-port-kit.md](PLAN/MILESTONES/M120-native-node-port-kit.md) |
| M121 | Native Routing And Mix | todo | M120, M101, M137 |  | E3 | [M121-native-routing-and-mix.md](PLAN/MILESTONES/M121-native-routing-and-mix.md) |
| M122 | Native Colour Utilities | todo | M120, M137 |  | E3 | [M122-native-colour-utilities.md](PLAN/MILESTONES/M122-native-colour-utilities.md) |
| M123 | Native Sharpen And Glow | todo | M120 |  | E3 | [M123-native-sharpen-and-glow.md](PLAN/MILESTONES/M123-native-sharpen-and-glow.md) |
| M124 | Native Time Remaps | todo | M120 |  | E3 | [M124-native-time-remaps.md](PLAN/MILESTONES/M124-native-time-remaps.md) |
| M125 | Native Multi-Frame Nodes | todo | M124 |  | E3 | [M125-native-multi-frame-nodes.md](PLAN/MILESTONES/M125-native-multi-frame-nodes.md) |
| M126 | Runtime GPU Shaders | done | M85 | gpu | E5 | [M126-runtime-gpu-shaders.md](PLAN/MILESTONES/M126-runtime-gpu-shaders.md) |
| M127 | Native OCIO Nodes | todo | M120, M126, M107 |  | E3 | [M127-native-ocio-nodes.md](PLAN/MILESTONES/M127-native-ocio-nodes.md) |
| M128 | Native Shape Generators | todo | M120 |  | E3 | [M128-native-shape-generators.md](PLAN/MILESTONES/M128-native-shape-generators.md) |
| M129 | Clean-Sheet Noise Node | todo | M120 |  | E3 | [M129-clean-sheet-noise-node.md](PLAN/MILESTONES/M129-clean-sheet-noise-node.md) |
| M130 | Native Edge-Preserving And Directional Filters | todo | M120 |  | E3 | [M130-native-edge-preserving-and-directional-filters.md](PLAN/MILESTONES/M130-native-edge-preserving-and-directional-filters.md) |
| M131 | Native Defocus Family | todo | M120 |  | E3 | [M131-native-defocus-family.md](PLAN/MILESTONES/M131-native-defocus-family.md) |
| M132 | Native Lens Distortion | todo | M113 |  | E3 | [M132-native-lens-distortion.md](PLAN/MILESTONES/M132-native-lens-distortion.md) |
| M133 | ZDefocus Lens Aberrations | todo | M131 |  | E3 | [M133-zdefocus-lens-aberrations.md](PLAN/MILESTONES/M133-zdefocus-lens-aberrations.md) |
| M117 | GUI Perf Harness And Baseline | todo | - |  | E10 | [M117-gui-perf-harness-and-baseline.md](PLAN/MILESTONES/M117-gui-perf-harness-and-baseline.md) |
| M118 | Large Graph GUI Responsiveness | todo | M117, M56 |  | E10 | [M118-large-graph-gui-responsiveness.md](PLAN/MILESTONES/M118-large-graph-gui-responsiveness.md) |
| M119 | Trackball Panel Layout | doing | M56, M69 | bosley-native-nodes | E10 | [M119-trackball-panel-layout.md](PLAN/MILESTONES/M119-trackball-panel-layout.md) |
| M134 | Output Picker Pixel Aspect | todo | - |  | E10 | [M134-output-picker-pixel-aspect.md](PLAN/MILESTONES/M134-output-picker-pixel-aspect.md) |
| M135 | Flaky Test Fixes | done | - | bosley-native-nodes | E2 | [M135-pool-sharing-flake-fix.md](PLAN/MILESTONES/M135-pool-sharing-flake-fix.md) |
| M136 | Edge Policy Investigation | todo | - |  | E3 | [M136-edge-policy-investigation.md](PLAN/MILESTONES/M136-edge-policy-investigation.md) |
| M137 | Native Base Class Flexibility | done | - | bosley-native-nodes | E3 | [M137-native-base-class-flexibility.md](PLAN/MILESTONES/M137-native-base-class-flexibility.md) |
| M138 | GUI Papercuts | blocked | - | bosley-native-nodes | E10 | [M138-gui-papercuts.md](PLAN/MILESTONES/M138-gui-papercuts.md) |
| M139 | Blur In-Graph Overhead | todo | M90, M137 |  | E6 | [M139-blur-in-graph-overhead.md](PLAN/MILESTONES/M139-blur-in-graph-overhead.md) |
| M140 | Native Metadata Utilities | todo | M77, M120 |  | E3 | [M140-native-metadata-utilities.md](PLAN/MILESTONES/M140-native-metadata-utilities.md) |
| M141 | Complete Local Build Environment | done | - | bosley-native-nodes | E2 | [M141-complete-local-build-environment.md](PLAN/MILESTONES/M141-complete-local-build-environment.md) |
| M142 | Atomic Project Save | todo | - |  | E7 | [M142-atomic-project-save.md](PLAN/MILESTONES/M142-atomic-project-save.md) |
| M143 | Bundled Plugin Bug Fixes | done | M100 | bosley-native-nodes | E3 | [M143-bundled-plugin-bug-fixes.md](PLAN/MILESTONES/M143-bundled-plugin-bug-fixes.md) |
| M144 | Render-Thread Metadata Refresh | todo | M100 |  | E14 | [M144-render-thread-metadata-refresh.md](PLAN/MILESTONES/M144-render-thread-metadata-refresh.md) |
| M145 | OFX Fork Plugin Purge | todo | M113, M121, M122, M123, M124, M125, M127, M128, M129, M130, M132, M140, M143 |  | E3 | [M145-ofx-fork-plugin-purge.md](PLAN/MILESTONES/M145-ofx-fork-plugin-purge.md) |
| M146 | Roto Hosting For Any Node | todo | M137 |  | E14 | [M146-roto-hosting-for-any-node.md](PLAN/MILESTONES/M146-roto-hosting-for-any-node.md) |
| M147 | SplineWarp Node Core | todo | M146, M113, M120, M137 |  | E3 | [M147-splinewarp-node-core.md](PLAN/MILESTONES/M147-splinewarp-node-core.md) |
| M148 | SplineWarp Viewer Tools | todo | M147 |  | E3 | [M148-splinewarp-viewer-tools.md](PLAN/MILESTONES/M148-splinewarp-viewer-tools.md) |
| M149 | SplineWarp Mesh Solver | todo | M147 |  | E3 | [M149-splinewarp-mesh-solver.md](PLAN/MILESTONES/M149-splinewarp-mesh-solver.md) |
| M150 | SplineWarp GPU Kernel | todo | M147 |  | E3 | [M150-splinewarp-gpu-kernel.md](PLAN/MILESTONES/M150-splinewarp-gpu-kernel.md) |
| M151 | SplineWarp Motion Blur | todo | M147, M150 |  | E3 | [M151-splinewarp-motion-blur.md](PLAN/MILESTONES/M151-splinewarp-motion-blur.md) |
| M152 | Curve Track Links And Roto Copy | todo | M148 |  | E3 | [M152-curve-track-links-and-roto-copy.md](PLAN/MILESTONES/M152-curve-track-links-and-roto-copy.md) |
| M153 | Distortion Concatenation | todo | M147, M113 |  | E14 | [M153-distortion-concatenation.md](PLAN/MILESTONES/M153-distortion-concatenation.md) |
| M154 | Fresh Buffer Page Zeroing | done | M109 | gpu | E5 | [M154-fresh-buffer-page-zeroing.md](PLAN/MILESTONES/M154-fresh-buffer-page-zeroing.md) |
| M155 | Per-Suite Test Processes | done | - | bosley-native-nodes | E2 | [M155-per-suite-test-processes.md](PLAN/MILESTONES/M155-per-suite-test-processes.md) |
| M156 | GPU Download Throughput In Comps | doing | M109 | gpu | E5 | [M156-gpu-download-throughput-in-comps.md](PLAN/MILESTONES/M156-gpu-download-throughput-in-comps.md) |
| M157 | Relicense To GPLv3 Or Later | todo | - |  | E2 | [M157-relicense-to-gplv3-or-later.md](PLAN/MILESTONES/M157-relicense-to-gplv3-or-later.md) |
| M158 | Node Toolbar As Menu | done | - | bosley-native-nodes | E10 | [M158-node-toolbar-as-menu.md](PLAN/MILESTONES/M158-node-toolbar-as-menu.md) |

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

# Open questions

- **M138 - GUI Papercuts:** paused at its UAT (2026-10-24, same rule as M16). Needs the user's sign-off on `build/appimages/M138-uat.md` with `build/appimages/M138-b9095c319.AppImage` (release, 334/334; GUI scripts 57/0 and 413/0). Resumes at the gate, PR, review and merge.
- **M16 - Project Format Redesign:** paused at P7.T3 UAT (2026-10-24, per the user's rule for this run: blocked on feedback → open question, pause, pick up the next E3 milestone). Needs the user's sign-off on `build/appimages/M16-uat.md` with `build/appimages/M16-ba521118c.AppImage` (release, 342/342 tests). Resumes at the gate, PR, review and merge.
- **M64 - Tiled Rendering:** dropped for now (user, 2026-10-08): this 4-core dev host is too slow for the Phase 64.7 re-evaluation benchmarks to be meaningful. Resume only on a faster host and with the user's go-ahead.
