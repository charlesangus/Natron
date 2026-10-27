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
| M43 | Drop Premult Concept | doing | - | bosley-native-nodes | E4 | [M43-drop-premult-concept.md](PLAN/MILESTONES/M43-drop-premult-concept.md) |
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
| M80 | Native RAW Support | done | M75, M85, M120, M137 | bosley-native-nodes | E3 | [M80-native-raw-support.md](PLAN/MILESTONES/M80-native-raw-support.md) |
| M159 | Kind-Agnostic Groups | done | - | bosley-native-nodes | E3 | [M159-kind-agnostic-groups.md](PLAN/MILESTONES/M159-kind-agnostic-groups.md) |
| M160 | Lensfun Lens Correction | todo | M80, M113 |  | E3 | [M160-lensfun-lens-correction.md](PLAN/MILESTONES/M160-lensfun-lens-correction.md) |
| M82 | Render Cost Profiling | done |  |  | E6 | [M82-render-cost-profiling.md](PLAN/MILESTONES/M82-render-cost-profiling.md) |
| M83 | GPU Compute Backend Spike | done |  |  | E5 | [M83-gpu-compute-backend-spike.md](PLAN/MILESTONES/M83-gpu-compute-backend-spike.md) |
| M84 | GPU Placement And Residency | done |  |  | E5 | [M84-gpu-placement-and-residency.md](PLAN/MILESTONES/M84-gpu-placement-and-residency.md) |
| M101 | Clean-Sheet Merge Node | doing | M84, M137, M158 | bosley-native-nodes | E3 | [M101-clean-sheet-merge-node.md](PLAN/MILESTONES/M101-clean-sheet-merge-node.md) |
| M111 | Native Node Identity Cleanup | todo | M76, M101, M120, M184 |  | E3 | [M111-native-node-identity-cleanup.md](PLAN/MILESTONES/M111-native-node-identity-cleanup.md) |
| M112 | Native Node Behaviour Fixes | done | - | bosley-native-nodes | E3 | [M112-native-node-behaviour-fixes.md](PLAN/MILESTONES/M112-native-node-behaviour-fixes.md) |
| M81 | Raw GPU Kernels | done | M80, M85 | gpu | E5 | [M81-raw-gpu-kernels.md](PLAN/MILESTONES/M81-raw-gpu-kernels.md) |
| M85 | Native GPU Kernels | done | M84 | gpu | E5 | [M85-native-gpu-kernels.md](PLAN/MILESTONES/M85-native-gpu-kernels.md) |
| M107 | GPU Viewer Hand-off | done | M85 | gpu | E5 | [M107-gpu-viewer-hand-off.md](PLAN/MILESTONES/M107-gpu-viewer-hand-off.md) |
| M108 | Retire Engine GL Host Path | done | M85 | gpu | E5 | [M108-retire-engine-gl-host-path.md](PLAN/MILESTONES/M108-retire-engine-gl-host-path.md) |
| M86 | OFX 1.5 GPU Suites | todo | M85, M108 |  | E5 | [M86-ofx-gpu-suites.md](PLAN/MILESTONES/M86-ofx-gpu-suites.md) |
| M87 | ~~Point-Op Kernel Fusion~~ | cancelled | M85, M126 | gpu | E5 | [M87-point-op-kernel-fusion.md](PLAN/MILESTONES/M87-point-op-kernel-fusion.md) |
| M109 | GPU Region Engine Hardening | done | M85 | gpu | E5 | [M109-gpu-region-engine-hardening.md](PLAN/MILESTONES/M109-gpu-region-engine-hardening.md) |
| M110 | Remaining Native GPU Ports | done | M85, M109, M126, M137 | gpu | E5 | [M110-remaining-native-gpu-ports.md](PLAN/MILESTONES/M110-remaining-native-gpu-ports.md) |
| M88 | FIR Gaussian Blur | done |  |  | E6 | [M88-fir-gaussian-blur.md](PLAN/MILESTONES/M88-fir-gaussian-blur.md) |
| M89 | Shared Worktree Ccache | done | - |  | E2 | [M89-shared-worktree-ccache.md](PLAN/MILESTONES/M89-shared-worktree-ccache.md) |
| M90 | Render Path Serial Overhead | done | M84 | bosley-native-nodes | E6 | [M90-render-path-serial-overhead.md](PLAN/MILESTONES/M90-render-path-serial-overhead.md) |
| M91 | NVIDIA GPU Validation | done | M84 | bosley-faux | E5 | [M91-nvidia-gpu-validation.md](PLAN/MILESTONES/M91-nvidia-gpu-validation.md) |
| M116 | GPU Region Overlay | todo | M81, M85, M91, M107, M108, M109, M110 |  | E5 | [M116-gpu-region-overlay.md](PLAN/MILESTONES/M116-gpu-region-overlay.md) |
| M27 | Real Debug Build | done | - | | E2 | [M27-debug-build-defines-ndebug.md](PLAN/MILESTONES/M27-debug-build-defines-ndebug.md) |
| M22 | Missing Plugin Placeholder | done | M75 | bosley-native-nodes | E3 | [M22-missing-plugin-placeholder.md](PLAN/MILESTONES/M22-missing-plugin-placeholder.md) |
| M31 | ~~Architectural Cleanup~~ | cancelled | M75, M76 |  | E14 | [M31-architectural-cleanup.md](PLAN/MILESTONES/M31-architectural-cleanup.md) |
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
| M19 | USD/Hydra Foundation | done | M17, M161, M107 |  | E9 | [M19-usd-hydra-foundation.md](PLAN/MILESTONES/M19-usd-hydra-foundation.md) |
| M20 | 3D Node Vocabulary | done | M19 | bosley-native-nodes | E9 | [M20-3d-node-vocabulary.md](PLAN/MILESTONES/M20-3d-node-vocabulary.md) |
| M226 | Scene Hierarchy From Topology | doing | M19, M20 | bosley-native-nodes | E9 | [M226-scene-hierarchy-from-topology.md](PLAN/MILESTONES/M226-scene-hierarchy-from-topology.md) |
| M227 | USD Primitive Nodes | todo | M226 |  | E9 | [M227-usd-primitive-nodes.md](PLAN/MILESTONES/M227-usd-primitive-nodes.md) |
| M228 | 3D Viewer Navigation | todo | M19, M20, M226 |  | E9 | [M228-3d-viewer-navigation.md](PLAN/MILESTONES/M228-3d-viewer-navigation.md) |
| M229 | 3D Viewer Guides And Display | todo | M228 |  | E9 | [M229-3d-viewer-guides-and-display.md](PLAN/MILESTONES/M229-3d-viewer-guides-and-display.md) |
| M230 | 3D Transform Widgets | todo | M226, M229 |  | E9 | [M230-3d-transform-widgets.md](PLAN/MILESTONES/M230-3d-transform-widgets.md) |
| M231 | Scene Graph Panel | todo | M229 |  | E9 | [M231-scene-graph-panel.md](PLAN/MILESTONES/M231-scene-graph-panel.md) |
| M232 | Scene Selection To Nodes | todo | M226, M231 |  | E9 | [M232-scene-selection-to-nodes.md](PLAN/MILESTONES/M232-scene-selection-to-nodes.md) |
| M233 | Live Node Textures | todo | M19, M20 |  | E9 | [M233-live-node-textures.md](PLAN/MILESTONES/M233-live-node-textures.md) |
| M234 | Camera Projection Nodes | todo | M227, M233 |  | E9 | [M234-camera-projection-nodes.md](PLAN/MILESTONES/M234-camera-projection-nodes.md) |
| M235 | USDCamera From File | todo | M226 |  | E9 | [M235-usdcamera-from-file.md](PLAN/MILESTONES/M235-usdcamera-from-file.md) |
| M236 | Shared Materials And Assignment | todo | M226 |  | E9 | [M236-shared-materials-and-assignment.md](PLAN/MILESTONES/M236-shared-materials-and-assignment.md) |
| M237 | CI Flake Fixes | done | - | bosley-native-nodes | E2 | [M237-ci-flake-fixes.md](PLAN/MILESTONES/M237-ci-flake-fixes.md) |
| M238 | EdgeExtend Options | todo | M123 |  | E3 | [M238-edgeextend-options.md](PLAN/MILESTONES/M238-edgeextend-options.md) |
| M239 | Weight-Based GPU Placement | doing | - | gpu | E5 | [M239-weight-based-gpu-placement.md](PLAN/MILESTONES/M239-weight-based-gpu-placement.md) |
| M240 | Float Trap And Denormal Audit | doing | - | gpu | E14 | [M240-float-trap-and-denormal-audit.md](PLAN/MILESTONES/M240-float-trap-and-denormal-audit.md) |
| M241 | Up Axis And Units Policy | todo | M226 |  | E9 | [M241-up-axis-policy.md](PLAN/MILESTONES/M241-up-axis-policy.md) |
| M242 | Cache Invalidation Audit | todo | M178, M185 |  | E14 | [M242-cache-invalidation-audit.md](PLAN/MILESTONES/M242-cache-invalidation-audit.md) |
| M243 | Startup Pane Paint On NVIDIA | todo | - |  | E10 | [M243-startup-pane-paint-on-nvidia.md](PLAN/MILESTONES/M243-startup-pane-paint-on-nvidia.md) |
| M244 | NVIDIA Test Fixes | todo | M239 |  | E5 | [M244-nvidia-test-fixes.md](PLAN/MILESTONES/M244-nvidia-test-fixes.md) |
| M245 | Write Path Variables | todo | - |  | E3 | [M245-write-path-variables.md](PLAN/MILESTONES/M245-write-path-variables.md) |
| M52 | USD Layer-Stack Completion | todo | M226 |  | E9 | [M52-usd-layer-stack-completion.md](PLAN/MILESTONES/M52-usd-layer-stack-completion.md) |
| M53 | Projected Texture Painting | todo | M52, M233 |  | E9 | [M53-projected-texture-painting.md](PLAN/MILESTONES/M53-projected-texture-painting.md) |
| M54 | SOPs-Style Geometry | todo | M52, M53, M227 |  | E9 | [M54-sops-style-geometry.md](PLAN/MILESTONES/M54-sops-style-geometry.md) |
| M21 | Deep Tier-2 And Bridges | todo |  |  | E8 | [M21-deep-tier2-and-bridges.md](PLAN/MILESTONES/M21-deep-tier2-and-bridges.md) |
| M51 | Deep Filtering OpenDCX | todo |  |  | E8 | [M51-deep-filtering-opendcx.md](PLAN/MILESTONES/M51-deep-filtering-opendcx.md) |
| M32 | Deep Sample Node | todo |  |  | E8 | [M32-deep-sample-node.md](PLAN/MILESTONES/M32-deep-sample-node.md) |
| M93 | Serialization Boundary | done | - | slate-ferret | E7 | [M93-serialization-boundary.md](PLAN/MILESTONES/M93-serialization-boundary.md) |
| M100 | Content Node Hashing | done | M93 | bosley-native-nodes | E7 | [M100-content-node-hashing.md](PLAN/MILESTONES/M100-content-node-hashing.md) |
| M16 | Project Format Redesign | done | M93 | bosley-native-nodes | E7 | [M16-project-file-format-redesign.md](PLAN/MILESTONES/M16-project-file-format-redesign.md) |
| M114 | Global Undo Correctness | done | M93 | bosley-native-nodes | E7 | [M114-global-undo-correctness.md](PLAN/MILESTONES/M114-global-undo-correctness.md) |
| M102 | Graph Edit Operations | todo | M16 |  | E7 | [M102-graph-edit-operations.md](PLAN/MILESTONES/M102-graph-edit-operations.md) |
| M103 | Graph Diff And Merge | todo | M102 |  | E12 | [M103-graph-diff-and-merge.md](PLAN/MILESTONES/M103-graph-diff-and-merge.md) |
| M104 | Merge Resolution UI | todo | M103 |  | E12 | [M104-merge-resolution-ui.md](PLAN/MILESTONES/M104-merge-resolution-ui.md) |
| M105 | Live Session Core | todo | M102 |  | E11 | [M105-live-session-core.md](PLAN/MILESTONES/M105-live-session-core.md) |
| M106 | Live Session Presence And Undo | todo | M105 |  | E11 | [M106-live-session-presence-and-undo.md](PLAN/MILESTONES/M106-live-session-presence-and-undo.md) |
| M70 | RFV Declaration And Storage | done |  |  | E13 | [M70-rfv-declaration-and-storage.md](PLAN/MILESTONES/M70-rfv-declaration-and-storage.md) |
| M71 | RFV Render Context | doing | M70, M178 | bosley-native-nodes | E13 | [M71-rfv-render-context.md](PLAN/MILESTONES/M71-rfv-render-context.md) |
| M72 | RFV Integration And UAT | todo |  |  | E13 | [M72-rfv-integration-and-uat.md](PLAN/MILESTONES/M72-rfv-integration-and-uat.md) |
| M92 | Linked Nodes | done | M24 | bosley-native-nodes | E10 | [M92-linked-nodes.md](PLAN/MILESTONES/M92-linked-nodes.md) |
| M95 | Knob Cleanup And Keyer Redesign | todo | M69, M76, M137 |  | E3 | [M95-knob-cleanup-and-keyer-redesign.md](PLAN/MILESTONES/M95-knob-cleanup-and-keyer-redesign.md) |
| M96 | Weighted Curve Tangents | done | M93, M16 | bosley-native-nodes | E7 | [M96-weighted-curve-tangents.md](PLAN/MILESTONES/M96-weighted-curve-tangents.md) |
| M97 | ~~Value-Based Node Hashing~~ | cancelled |  |  | E7 | [M97-value-based-node-hashing.md](PLAN/MILESTONES/M97-value-based-node-hashing.md) |
| M94 | Delete Unused Maintainer Skill | done | - | bosley-native-nodes | E2 | [M94-delete-unused-maintainer-skill.md](PLAN/MILESTONES/M94-delete-unused-maintainer-skill.md) |
| M115 | Build Speed And Artifact Size | done | - | bosley-native-nodes | E2 | [M115-build-speed-and-artifact-size.md](PLAN/MILESTONES/M115-build-speed-and-artifact-size.md) |
| M113 | Native Warp Nodes | done | M120, M56 | bosley-native-nodes | E3 | [M113-native-node-rewrites-continued.md](PLAN/MILESTONES/M113-native-node-rewrites-continued.md) |
| M120 | Native Node Port Kit | done | M85, M137 | bosley-native-nodes | E3 | [M120-native-node-port-kit.md](PLAN/MILESTONES/M120-native-node-port-kit.md) |
| M121 | Native Routing And Mix | todo | M120, M101, M137 |  | E3 | [M121-native-routing-and-mix.md](PLAN/MILESTONES/M121-native-routing-and-mix.md) |
| M122 | Native Colour Utilities | done | M120, M137 |  | E3 | [M122-native-colour-utilities.md](PLAN/MILESTONES/M122-native-colour-utilities.md) |
| M123 | Native Sharpen And Glow | doing | M120 | bosley-native-nodes | E3 | [M123-native-sharpen-and-glow.md](PLAN/MILESTONES/M123-native-sharpen-and-glow.md) |
| M124 | Native Time Remaps | todo | M120 |  | E3 | [M124-native-time-remaps.md](PLAN/MILESTONES/M124-native-time-remaps.md) |
| M125 | Native Multi-Frame Nodes | todo | M124 |  | E3 | [M125-native-multi-frame-nodes.md](PLAN/MILESTONES/M125-native-multi-frame-nodes.md) |
| M126 | Runtime GPU Shaders | done | M85 | gpu | E5 | [M126-runtime-gpu-shaders.md](PLAN/MILESTONES/M126-runtime-gpu-shaders.md) |
| M127 | Native OCIO Nodes | done | M120, M126, M107 |  | E3 | [M127-native-ocio-nodes.md](PLAN/MILESTONES/M127-native-ocio-nodes.md) |
| M128 | Native Shape Generators | todo | M120 |  | E3 | [M128-native-shape-generators.md](PLAN/MILESTONES/M128-native-shape-generators.md) |
| M129 | Clean-Sheet Noise Node | done | M120 |  | E3 | [M129-clean-sheet-noise-node.md](PLAN/MILESTONES/M129-clean-sheet-noise-node.md) |
| M130 | Native Edge-Preserving And Directional Filters | todo | M120 |  | E3 | [M130-native-edge-preserving-and-directional-filters.md](PLAN/MILESTONES/M130-native-edge-preserving-and-directional-filters.md) |
| M131 | Native Defocus Family | todo | M120 |  | E3 | [M131-native-defocus-family.md](PLAN/MILESTONES/M131-native-defocus-family.md) |
| M132 | Native Lens Distortion | done | M113 |  | E3 | [M132-native-lens-distortion.md](PLAN/MILESTONES/M132-native-lens-distortion.md) |
| M133 | ZDefocus Lens Aberrations | todo | M131 |  | E3 | [M133-zdefocus-lens-aberrations.md](PLAN/MILESTONES/M133-zdefocus-lens-aberrations.md) |
| M117 | GUI Perf Harness And Baseline | todo | - |  | E10 | [M117-gui-perf-harness-and-baseline.md](PLAN/MILESTONES/M117-gui-perf-harness-and-baseline.md) |
| M118 | Large Graph GUI Responsiveness | todo | M117, M56 |  | E10 | [M118-large-graph-gui-responsiveness.md](PLAN/MILESTONES/M118-large-graph-gui-responsiveness.md) |
| M119 | Trackball Panel Layout | done | M56, M69 | bosley-native-nodes | E10 | [M119-trackball-panel-layout.md](PLAN/MILESTONES/M119-trackball-panel-layout.md) |
| M134 | Output Picker Pixel Aspect | done | - | bosley-native-nodes | E10 | [M134-output-picker-pixel-aspect.md](PLAN/MILESTONES/M134-output-picker-pixel-aspect.md) |
| M135 | Flaky Test Fixes | done | - | bosley-native-nodes | E2 | [M135-pool-sharing-flake-fix.md](PLAN/MILESTONES/M135-pool-sharing-flake-fix.md) |
| M136 | Edge Policy Investigation | todo | M167 |  | E3 | [M136-edge-policy-investigation.md](PLAN/MILESTONES/M136-edge-policy-investigation.md) |
| M137 | Native Base Class Flexibility | done | - | bosley-native-nodes | E3 | [M137-native-base-class-flexibility.md](PLAN/MILESTONES/M137-native-base-class-flexibility.md) |
| M138 | GUI Papercuts | done | - | bosley-native-nodes | E10 | [M138-gui-papercuts.md](PLAN/MILESTONES/M138-gui-papercuts.md) |
| M139 | Blur In-Graph Overhead | todo | M90, M137 |  | E6 | [M139-blur-in-graph-overhead.md](PLAN/MILESTONES/M139-blur-in-graph-overhead.md) |
| M140 | Native Metadata Utilities | todo | M77, M120 |  | E3 | [M140-native-metadata-utilities.md](PLAN/MILESTONES/M140-native-metadata-utilities.md) |
| M141 | Complete Local Build Environment | done | - | bosley-native-nodes | E2 | [M141-complete-local-build-environment.md](PLAN/MILESTONES/M141-complete-local-build-environment.md) |
| M142 | Atomic Project Save | done | - | bosley-native-nodes | E7 | [M142-atomic-project-save.md](PLAN/MILESTONES/M142-atomic-project-save.md) |
| M143 | Bundled Plugin Bug Fixes | done | M100 | bosley-native-nodes | E3 | [M143-bundled-plugin-bug-fixes.md](PLAN/MILESTONES/M143-bundled-plugin-bug-fixes.md) |
| M144 | ~~Render-Thread Metadata Refresh~~ | cancelled | M100 |  | E14 | [M144-render-thread-metadata-refresh.md](PLAN/MILESTONES/M144-render-thread-metadata-refresh.md) |
| M145 | OFX Fork Plugin Purge | todo | M113, M121, M122, M123, M124, M125, M127, M128, M129, M130, M132, M140, M143 |  | E3 | [M145-ofx-fork-plugin-purge.md](PLAN/MILESTONES/M145-ofx-fork-plugin-purge.md) |
| M146 | Roto Hosting For Any Node | done | M137 | bosley-native-nodes | E14 | [M146-roto-hosting-for-any-node.md](PLAN/MILESTONES/M146-roto-hosting-for-any-node.md) |
| M147 | SplineWarp Node Core | todo | M146, M113, M120, M137 |  | E3 | [M147-splinewarp-node-core.md](PLAN/MILESTONES/M147-splinewarp-node-core.md) |
| M148 | SplineWarp Viewer Tools | todo | M147 |  | E3 | [M148-splinewarp-viewer-tools.md](PLAN/MILESTONES/M148-splinewarp-viewer-tools.md) |
| M149 | SplineWarp Mesh Solver | todo | M147 |  | E3 | [M149-splinewarp-mesh-solver.md](PLAN/MILESTONES/M149-splinewarp-mesh-solver.md) |
| M150 | SplineWarp GPU Kernel | todo | M147 |  | E3 | [M150-splinewarp-gpu-kernel.md](PLAN/MILESTONES/M150-splinewarp-gpu-kernel.md) |
| M151 | SplineWarp Motion Blur | todo | M147, M150 |  | E3 | [M151-splinewarp-motion-blur.md](PLAN/MILESTONES/M151-splinewarp-motion-blur.md) |
| M152 | Curve Track Links And Roto Copy | todo | M148 |  | E3 | [M152-curve-track-links-and-roto-copy.md](PLAN/MILESTONES/M152-curve-track-links-and-roto-copy.md) |
| M153 | Distortion Concatenation | todo | M147, M113 |  | E14 | [M153-distortion-concatenation.md](PLAN/MILESTONES/M153-distortion-concatenation.md) |
| M154 | Fresh Buffer Page Zeroing | done | M109 | gpu | E5 | [M154-fresh-buffer-page-zeroing.md](PLAN/MILESTONES/M154-fresh-buffer-page-zeroing.md) |
| M155 | Per-Suite Test Processes | done | - | bosley-native-nodes | E2 | [M155-per-suite-test-processes.md](PLAN/MILESTONES/M155-per-suite-test-processes.md) |
| M156 | GPU Download Throughput In Comps | done | M109 | gpu | E5 | [M156-gpu-download-throughput-in-comps.md](PLAN/MILESTONES/M156-gpu-download-throughput-in-comps.md) |
| M157 | Relicense To GPLv3 Or Later | todo | - |  | E2 | [M157-relicense-to-gplv3-or-later.md](PLAN/MILESTONES/M157-relicense-to-gplv3-or-later.md) |
| M158 | Node Toolbar As Menu | done | - | bosley-native-nodes | E10 | [M158-node-toolbar-as-menu.md](PLAN/MILESTONES/M158-node-toolbar-as-menu.md) |
| M161 | Startup Compositor Crash | done | - | bosley-native-nodes | E10 | [M161-startup-compositor-crash.md](PLAN/MILESTONES/M161-startup-compositor-crash.md) |
| M170 | Engine Render Races | done | - | bosley-native-nodes | E14 | [M170-engine-render-races.md](PLAN/MILESTONES/M170-engine-render-races.md) |
| M162 | OFX CUDA And OpenCL Suites | todo | M86 |  | E5 | [M162-ofx-cuda-and-opencl-suites.md](PLAN/MILESTONES/M162-ofx-cuda-and-opencl-suites.md) |
| M171 | GPU Correctness Fixes | done | - | gpu | E5 | [M171-gpu-correctness-fixes.md](PLAN/MILESTONES/M171-gpu-correctness-fixes.md) |
| M163 | Incremental Bezier Edits | todo | - |  | E6 | [M163-incremental-bezier-edits.md](PLAN/MILESTONES/M163-incremental-bezier-edits.md) |
| M164 | Viewer Node Knobs | todo | M16, M107 |  | E3 | [M164-viewer-node-knobs.md](PLAN/MILESTONES/M164-viewer-node-knobs.md) |
| M165 | Rich Viewer Overlays | todo | M80 |  | E3 | [M165-rich-viewer-overlays.md](PLAN/MILESTONES/M165-rich-viewer-overlays.md) |
| M166 | Non-Float Plane Model | todo | - |  | E14 | [M166-non-float-plane-model.md](PLAN/MILESTONES/M166-non-float-plane-model.md) |
| M172 | GL Widget Context Loss | done | - | bosley-native-nodes | E10 | [M172-gl-widget-context-loss.md](PLAN/MILESTONES/M172-gl-widget-context-loss.md) |
| M167 | Per-Frame Node Metadata | done | M100 |  | E14 | [M167-per-frame-node-metadata.md](PLAN/MILESTONES/M167-per-frame-node-metadata.md) |
| M168 | NVIDIA Viewer Hand-off Validation | done | M107, M91 | bosley-faux | E5 | [M168-nvidia-viewer-hand-off-validation.md](PLAN/MILESTONES/M168-nvidia-viewer-hand-off-validation.md) |
| M169 | Pipelined Strip Member Timing | done | M107, M91 | bosley-faux | E5 | [M169-pipelined-strip-member-timing.md](PLAN/MILESTONES/M169-pipelined-strip-member-timing.md) |
| M173 | Global Mode For Value Knobs | todo | M119 |  | E10 | [M173-global-mode-for-value-knobs.md](PLAN/MILESTONES/M173-global-mode-for-value-knobs.md) |
| M174 | Wayland Support | blocked | - |  | E1 | [M174-wayland-support.md](PLAN/MILESTONES/M174-wayland-support.md) |
| M175 | Build Script Jobs And Test Legs | todo | - |  | E2 | [M175-build-script-jobs-and-test-legs.md](PLAN/MILESTONES/M175-build-script-jobs-and-test-legs.md) |
| M176 | Splice-Vertical Regression | todo | - | | E10 | [M176-splice-vertical-regression.md](PLAN/MILESTONES/M176-splice-vertical-regression.md) |
| M178 | Render Context Core | doing | M167 | bosley-native-nodes | E13 | [M178-render-context-core.md](PLAN/MILESTONES/M178-render-context-core.md) |
| M179 | Render Graph Continuation | todo | M178 |  | E6 | [M179-render-graph-continuation.md](PLAN/MILESTONES/M179-render-graph-continuation.md) |
| M180 | Loop Block Core | todo | M178 |  | E15 | [M180-loop-block-core.md](PLAN/MILESTONES/M180-loop-block-core.md) |
| M181 | Loop Drivers And UI | todo | M180, M71 |  | E15 | [M181-loop-drivers-and-ui.md](PLAN/MILESTONES/M181-loop-drivers-and-ui.md) |
| M182 | Loop Feedback | todo | M180 |  | E15 | [M182-loop-feedback.md](PLAN/MILESTONES/M182-loop-feedback.md) |
| M183 | Loop Map Blend | todo | M180, M179, M184 |  | E15 | [M183-loop-map-blend.md](PLAN/MILESTONES/M183-loop-map-blend.md) |
| M184 | Variable-Size Filter Kit | done | M120, M137 | gpu | E15 | [M184-variable-size-filter-kit.md](PLAN/MILESTONES/M184-variable-size-filter-kit.md) |
| M185 | Variable Warps | doing | M113, M184 | bosley-native-nodes | E15 | [M185-variable-warps.md](PLAN/MILESTONES/M185-variable-warps.md) |
| M186 | Exact IDefocus | todo | M131, M184 |  | E15 | [M186-exact-idefocus.md](PLAN/MILESTONES/M186-exact-idefocus.md) |
| M187 | Variable Sharpen And Glow | todo | M123, M184 |  | E15 | [M187-variable-sharpen-and-glow.md](PLAN/MILESTONES/M187-variable-sharpen-and-glow.md) |
| M188 | Variable Median And DirBlur | todo | M130, M184 |  | E15 | [M188-variable-median-and-dirblur.md](PLAN/MILESTONES/M188-variable-median-and-dirblur.md) |
| M189 | Make I-Node Action | todo | M183, M184, M181 |  | E15 | [M189-make-i-node-action.md](PLAN/MILESTONES/M189-make-i-node-action.md) |
| M190 | Cross-Frame Feedback | todo | M182 |  | E15 | [M190-cross-frame-feedback.md](PLAN/MILESTONES/M190-cross-frame-feedback.md) |
| M191 | Native VectorBlur | todo | M120, M137 |  | E3 | [M191-native-vectorblur.md](PLAN/MILESTONES/M191-native-vectorblur.md) |
| M177 | Renders Isolated From Edits | done | M167 |  | E14 | [M177-renders-isolated-from-edits.md](PLAN/MILESTONES/M177-renders-isolated-from-edits.md) |
| M192 | Reliable Pipe Pick-Up | done | - |  | E10 | [M192-reliable-pipe-pick-up.md](PLAN/MILESTONES/M192-reliable-pipe-pick-up.md) |
| M193 | Pipe Hint Highlight Colour | todo | M176 |  | E10 | [M193-pipe-hint-highlight-colour.md](PLAN/MILESTONES/M193-pipe-hint-highlight-colour.md) |
| M194 | Kiss Connections | todo | M192, M193 |  | E10 | [M194-kiss-connections.md](PLAN/MILESTONES/M194-kiss-connections.md) |
| M195 | Shake And Cut Disconnect | todo | M194 |  | E10 | [M195-shake-and-cut-disconnect.md](PLAN/MILESTONES/M195-shake-and-cut-disconnect.md) |
| M196 | Copy-Compute Contention | todo | M156 |  | E5 | [M196-copy-compute-contention.md](PLAN/MILESTONES/M196-copy-compute-contention.md) |
| M197 | Python Undo Step API | todo | M114 |  | E7 | [M197-python-undo-step-api.md](PLAN/MILESTONES/M197-python-undo-step-api.md) |
| M198 | RenderEngine On Node | todo | M177 |  | E14 | [M198-renderengine-on-node.md](PLAN/MILESTONES/M198-renderengine-on-node.md) |
| M199 | Remove Legacy Scheduler | todo | - |  | E14 | [M199-remove-legacy-scheduler.md](PLAN/MILESTONES/M199-remove-legacy-scheduler.md) |
| M200 | Folded Knob Shared Storage | todo | - |  | E14 | [M200-folded-knob-shared-storage.md](PLAN/MILESTONES/M200-folded-knob-shared-storage.md) |
| M201 | Trimap Removal | todo | M199 |  | E14 | [M201-trimap-removal.md](PLAN/MILESTONES/M201-trimap-removal.md) |
| M202 | Clean-Hash Cache Stores | doing | M167 | bosley-native-nodes | E14 | [M202-clean-hash-cache-stores.md](PLAN/MILESTONES/M202-clean-hash-cache-stores.md) |
| M203 | ONNX Inference Backend | todo | - |  | E16 | [M203-onnx-inference-backend.md](PLAN/MILESTONES/M203-onnx-inference-backend.md) |
| M204 | AI Model Manager | todo | M203 |  | E16 | [M204-ai-model-manager.md](PLAN/MILESTONES/M204-ai-model-manager.md) |
| M205 | Object Mask To Roto | todo | M203, M204, M146 |  | E16 | [M205-object-mask-to-roto.md](PLAN/MILESTONES/M205-object-mask-to-roto.md) |
| M206 | Neural Raw Denoise | todo | M203, M204 |  | E16 | [M206-neural-raw-denoise.md](PLAN/MILESTONES/M206-neural-raw-denoise.md) |
| M207 | Neural Denoise | todo | M203, M204 |  | E16 | [M207-neural-denoise.md](PLAN/MILESTONES/M207-neural-denoise.md) |
| M208 | Neural Upscale | todo | M203, M204, M207 |  | E16 | [M208-neural-upscale.md](PLAN/MILESTONES/M208-neural-upscale.md) |
| M209 | Edge-Aware Filter Library | todo | - |  | E17 | [M209-edge-aware-filter-library.md](PLAN/MILESTONES/M209-edge-aware-filter-library.md) |
| M210 | Profiled Denoise | todo | M209 |  | E17 | [M210-profiled-denoise.md](PLAN/MILESTONES/M210-profiled-denoise.md) |
| M211 | Wavelet Retouch | todo | M209, M146 |  | E17 | [M211-wavelet-retouch.md](PLAN/MILESTONES/M211-wavelet-retouch.md) |
| M212 | Tone Equalizer | todo | M209 |  | E17 | [M212-tone-equalizer.md](PLAN/MILESTONES/M212-tone-equalizer.md) |
| M213 | Contrast And Texture | todo | M209 |  | E17 | [M213-contrast-and-texture.md](PLAN/MILESTONES/M213-contrast-and-texture.md) |
| M214 | Colour Balance RGB | todo | - |  | E17 | [M214-colour-balance-rgb.md](PLAN/MILESTONES/M214-colour-balance-rgb.md) |
| M215 | Colour Equalizer | todo | M209, M214 |  | E17 | [M215-colour-equalizer.md](PLAN/MILESTONES/M215-colour-equalizer.md) |
| M216 | Auto-Straighten | todo | - |  | E17 | [M216-auto-straighten.md](PLAN/MILESTONES/M216-auto-straighten.md) |
| M217 | VectorGenerator Node | todo | M124 |  | E18 | [M217-vectorgenerator-node.md](PLAN/MILESTONES/M217-vectorgenerator-node.md) |
| M218 | VectorGenerator GPU | todo | M217, M124 |  | E18 | [M218-vectorgenerator-gpu.md](PLAN/MILESTONES/M218-vectorgenerator-gpu.md) |
| M219 | Curves Follow Vectors | todo | M217, M146 |  | E18 | [M219-curves-follow-vectors.md](PLAN/MILESTONES/M219-curves-follow-vectors.md) |
| M220 | SplineWarp Follow Vectors | todo | M219, M148 |  | E18 | [M220-splinewarp-follow-vectors.md](PLAN/MILESTONES/M220-splinewarp-follow-vectors.md) |
| M221 | Neural Optical Flow | todo | M217, M203, M204 |  | E16 | [M221-neural-optical-flow.md](PLAN/MILESTONES/M221-neural-optical-flow.md) |
| M222 | User Control Of GPU Placement | todo | M132 |  | E5 | [M222-user-control-of-gpu-placement.md](PLAN/MILESTONES/M222-user-control-of-gpu-placement.md) |
| M223 | Read And Write Knob Cleanup | todo | M167 |  | E3 | [M223-read-and-write-knob-cleanup.md](PLAN/MILESTONES/M223-read-and-write-knob-cleanup.md) |
| M224 | Fluid Roto And RotoPaint Interaction | todo | M146 |  | E6 | [M224-fluid-roto-and-rotopaint-interaction.md](PLAN/MILESTONES/M224-fluid-roto-and-rotopaint-interaction.md) |
| M225 | Clean Shutdown And Memory Release | doing | - | bosley-native-nodes | E14 | [M225-clean-shutdown-and-memory-release.md](PLAN/MILESTONES/M225-clean-shutdown-and-memory-release.md) |

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

# Open questions

- **M123 - Native Sharpen And Glow, Bloom design:** the user requires GPU residency (no CPU fallback) if at all possible; candidates are being spiked. Filter default (FIR Gaussian, recommended, or CImg's quasi-Gaussian) still open.
- **M198 - RenderEngine On Node, M199 - Remove Legacy Scheduler, M200 - Folded Knob Shared Storage, M201 - Trimap Removal:** do not start without the user's go-ahead (user, 2026-10-25, M31 triage). Each stub's `Blocked on:` names what the user is waiting for. M202 - Clean-Hash Cache Stores needs no go-ahead.
- **M174 - Wayland Support:** blocked until the user approves starting it (user, 2026-10-24). Stub; elaborate on approval.


- **M64 - Tiled Rendering:** dropped for now (user, 2026-10-08): this 4-core dev host is too slow for the Phase 64.7 re-evaluation benchmarks to be meaningful. Resume only on a faster host and with the user's go-ahead.
- **M166 - Non-Float Plane Model:** the user wants to rethink the model before it is planned: Read emitting EXR half/uint32 layers natively, and possibly a bit depth per layer. Settle the framing in `/cat-discuss`, then elaborate.
