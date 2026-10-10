---
title: Linux-Only Qt6 Foundation Plan
status: running
current: M75.P1.T5
pm_heartbeat: 2026-10-09T20:07:10-04:00
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
  **M3 shipped 2026-09-01** (PR #9, merge `d0349b060`): ACES 2.0 Studio is the
  default OCIO config, openfx-misc builds from pinned source in CI, and the dead
  Windows/macOS/qmake tree is gone.
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

- **M16 is authored but deferred** (2026-09-05): the `.ntp` format redesign is
  future work — do not start or elaborate it without an explicit user
  go-ahead. Design and evidence live in
  `PLAN/DECISIONS/2026-09-05-project-format-bundle-design.md`.
- **M17–M21 (deep compositing + USD/Hydra 3D)** authored 2026-09-05 from
  `PLAN/DESIGN/2026-09-05-deep-and-3d-native-extensions.md`, which governs on
  any brief's ambiguity. Sequencing: **M17 first** (typed-edge/native-node
  foundation), then **M18 and M19 in either order or in parallel**, then
  **M20** (needs M19's gate), then **M21** (stub; blocked on M18/M20
  real-world results and a design-doc amendment). Substrate decision:
  `PLAN/DECISIONS/2026-09-05-adopt-typed-edges-and-usd-substrate.md`.
- **M29–M30 (independent repo + automated beta releases)** authored
  2026-09-11, no ordering dependency between them — either can run first, or
  in parallel with the deep/3D work above. M29 recreates `origin` as a
  non-fork repo (chosen over a GitHub Support ticket — see M29's Decisions);
  M30 makes `release.yml`'s existing build/package pipeline fire
  automatically on every merge to `main`, tagged `v0.1.0-betaN` (sequential
  from 1) as a pre-release, alongside the unchanged manual stable-tag path.
- **M31 (architectural cleanup) is authored but deferred** (2026-09-15): a
  parking place for structural debts that feature work exposes — first entry
  is moving the `RenderEngine` from the `OutputEffectInstance` base class to
  `Node` by composition, surfaced by M18.P3.T8a. Do not start it without an
  explicit user go-ahead; add tasks to it as they surface instead of folding
  refactors into feature milestones.
- **M32 (deep sample inspector node) authored 2026-09-18** as a stub: a
  Nuke-style `DeepSample` node (picker + table) replacing M18.P2.T3's
  hover/tooltip probe. Needs a codebase-scouting pass (overlay-handle and
  panel-widget precedent) before elaboration — see
  `PLAN/DECISIONS/2026-09-18-deepsample-node-replaces-hover-probe.md`.
- **M52–M54 (3D roadmap) authored 2026-09-18** from
  `PLAN/DESIGN/2026-09-18-3d-roadmap-lops-paint-sops.md`, which amends the
  2026-09-05 design doc's Part 2 and governs on any brief's ambiguity.
  Order is fixed by user priority: **M52** (USD layer-stack completion:
  schema-driven edits, path-expression selection, composition nodes) after
  M20's gate → **M53** (projected texture painting: CPU UV bake,
  ProjectTexture, live textures into Hydra) after M52 → **M54** (SOPs-style
  geometry: `eDataKindGeometry`, `GeoDetail`, bridges, operators, point
  editing) after M53. Geometry as a fourth data kind is decided in
  `PLAN/DECISIONS/2026-09-18-geometry-is-a-fourth-data-kind.md`.
- **Backlog reorg and prioritization (2026-09-18)**: after a `/cat-discuss`
  review, execution order is channel/layer rework (M39, M34, M35, M36, M37,
  M38, M50) → compositing semantics (M43) → infra/housekeeping (M25, M27,
  M28, M22, M29, M30) → polish (M24, M44, M55, M56) → the 3D roadmap (M19,
  M20, M52, M53, M54). Deferred with no active order: deep work (M21, M51,
  M32), Nuke-tool ports (M45, M46), M16, M31. See
  `DECISIONS/2026-09-18-backlog-reorg-and-prioritization.md`.
  **Reordered again (2026-09-19, via inbox):** M57 (Write node regressions
  from M39 testing) goes first, and M43 (drop premult/unpremult) moves ahead
  of M34 so compositing semantics land alongside the rest of the channel/layer
  rework rather than after it. New sequence: M57 → M35 → M38 → M43 → M34 →
  M36 → M37 → M50 → (unchanged: infra/housekeeping → polish → 3D roadmap).
  **M39 shipped 2026-09-19** (PR #26, merge `1e59109fe`; docs PR #25): the
  Natron side of the codebase says "layer" everywhere; only the OpenFX ABI
  boundary keeps "plane". `.ntp` tags are `LayerID`/`LayerLabel`, the knob
  script-name is `processAllLayers`, `NATRON_CACHE_VERSION` is 5.
  **User testing of M39 (2026-09-19) surfaced two Write-node bugs, filed as
  new milestone M57**: the Write node's params panel still shows "All
  Planes" (this repo's own allowlist grep is clean — `WriteNode::
  getCreateChannelSelectorKnob()` returns `false`, so the only "process
  everything" checkbox on a Write is the embedded OFX writer's own
  `kMultiPlaneProcessAllPlanesParam`, defined in the separate
  `charlesangus/openfx-io` fork's `SupportExt/ofxsMultiPlane.h`, untouched by
  M39), and checking that box does not write every input layer to the
  output. **M57 shipped 2026-09-19**: the label fix landed (M57.P1.T1,
  `charlesangus/openfx-io` PR #3, `OPENFX_IO_REF` bumped to `87264e5`); the
  second bug (M57.P1.T2) bisected as **pre-existing, not an M39 regression**
  (byte-identical wrong-pixel-data symptom on `3e14c2a1e`, pre-M39) and was
  rescoped out to new stub milestone **M58** rather than fixed here — see
  M57's `## Decisions`. **M58 placed first (2026-09-19, user decision):** it
  runs ahead of M35 so the Write-node correctness bug is closed before the
  rest of the channel/layer rework; the sequence after it is unchanged.
  **M58 shipped 2026-09-19** (PR #28, squash-merge `b2d4ef1d2`): a Read/Write
  container's `knobsAge` is folded into its embedded node's hash, so toggling
  All Layers after a render now invalidates the encoder's cached plane set.
  **M59 (pre-commit auto-format hook) added 2026-09-19, up next after M58:**
  small housekeeping milestone — PRs keep failing CI's `format` job because
  the existing `.git-hooks/pre-commit` is check-only, uninstalled, and passes
  silently when clang-format is absent. Runs before M35. **Shipped 2026-09-19**
  (PR #29, squash-merge `c8e64551b`): `tools/install-git-hooks.sh` links a hook
  that auto-formats staged C/C++ with the pinned clang-format 21.1.8 and
  refuses the commit when the tool is missing.
  **M35 moved back behind M34 (2026-09-19, user decision):** its stub is
  blocked on the new Shuffle node existing as the replacement path, so the
  sequence is now M38 → M43 → M34 → M36 → M37 → M50. **Then M35 was folded
  into M38 (same day, user decision)**: M38 is no longer a layout reorder but a
  new Nuke-style-but-stronger layer/channel widget (dropdown None/All/Regex/
  layers + dynamic channel buttons, multiple instances via "Add layer"),
  with nodes processing exactly the selected layers/channels in place and no
  shuffle capability outside the Shuffle node. See M38's `## Decisions`.
  **M38 UAT round 1 (2026-09-21) did not pass**: the user's findings and
  design feedback became **Phase 38.10** (14 tasks, placed before Phase 38.9
  in the file so the PM runs it first), then 38.9 re-takes the after-shots
  (T1a) and re-runs the checkpoint (T2, round 2). **Shipped 2026-09-22** (PR #30, squash-merge `30c1f0ceb`) after one Codex review round (4 fixed, 2 declined).
  **M34 (Shuffle) runs next, M60 moved back behind M50 (2026-09-22, user decision).**
  **M34 shipped 2026-09-24** (PR #33, squash-merge `b0e212d5a`) after two Codex rounds; P6.T11, the user's check of the AppImage, is still outstanding. **M30 merged right after it** (PR #31, `e8e3c96c4`), so automatic beta releases are armed.
  **M61 added 2026-09-24 and runs next, ahead of M37 (user decision):** M34's round-2 review wrongly declined a render-time validation test because it assumed layers never vary with time. M61 makes per-frame AOV sequences, an animated Switch and an animated Disable each vary a node's layers per frame, fixing whichever doesn't, and then adds that test. See `DECISIONS/2026-09-24-layers-vary-with-time.md`.
  **M61 PR #34 opened 2026-09-26** (CI green, Codex round closed); awaiting the user's check of the disabled-cross width and the UAT on `build/appimages/M61-d92ef11e3.AppImage`. openfx-io#7 is pinned by its branch commit; re-pin it after it merges. **M65 is next** (rgba/rgb/alpha/xy colour views, added 2026-09-26), stacked on M61's branch; M37 then stacks on M65.
  **Parcel run (2026-10-02, user):** M66 → M37 → M50 → M60 run autonomously, each stacked on the previous branch (M66 on M65's tip). The user UATs the whole parcel, M61 through M60, then merges bottom-up. M65 is `blocked` awaiting that UAT.
  M43 and M36 were absorbed into M38 as Phases 38.2 and 38.7 — their rows are
  cancelled like M35's.

- **M67 (rewrite core nodes as native nodes) added 2026-10-03 as a stub** after M62.P5.T1 showed 75–80% of per-node memory is OpenFX-specific; its first task is the native-vs-OFX chain benchmark. The memory proposals went to M31 Phase 31.2 as speculative. See `DECISIONS/2026-10-03-native-core-nodes-over-ofx-memory-fixes.md`.
- **M62–M64 (performance and render strategy) added 2026-09-25** from a side session's benchmarks (`tools/bench/`, results in `build/bench/`). Runs **after the channel/layer work (M61, M37, M50, M60) and ahead of infra/housekeeping** (user decision). M62 fixes the superlinear engine hotspots and commits the bench as the regression harness. M63 (task-graph scheduler) and M64 (tiles) are stubs, each blocked on the previous milestone's re-benchmark. See `DECISIONS/2026-09-25-perf-hotspots-before-render-architecture.md`.
- **Stacked milestone PRs (2026-09-22, user):** from M34 on, every milestone packages an AppImage to `build/appimages/`, opens its PR against the previous milestone's branch, runs its review round, and stays open. The next milestone branches off the previous tip, and the user checks and merges asynchronously. Fixes from a user check are merged up the stack, never rebased. See `DECISIONS/2026-09-22-stacked-milestone-prs.md`.
- **Parallel milestones in worktrees (2026-09-23, user):** M28 and M30 run alongside M34 in worktrees under `build/wt/` (inside the container mount), each branched off `main` and PR'd against `main`, not stacked. Builds still serialize through the one natron-dev container. See `DECISIONS/2026-09-23-parallel-worktree-milestones.md`.

- **Parcel 2 (2026-10-06, user):** parcel 1 (M61 → M62, PRs #34–#40) passed UAT and **merged 2026-10-06** (squashes a83bc9a7f … 1c2f9d62e on `main`; fork PRs merged with merge commits and re-pinned in d1111ef8f, which rode #40). #41 now targets `main` (head ce575b4d2). Next, M64 (tiles), then M67 (native core nodes: colour → merge/generators → spatial → keying/misc), each stacked on the previous branch starting from M63's #41, for one UAT. **Parcel 2 merged 2026-10-08:** M63 as PR #41 (squash `d4beebbf6`, 12:10Z) and M67 as PR #42 (squash `c1eb2fd1b`, 13:36Z), both on `main`; M64 stayed parked. M68 (headless GL, ex-M63.P5.T4) is deferred until the user gives the go-ahead. See `DECISIONS/2026-10-06-m67-core-node-families.md`.

- **M70–M72 (Reverse-Flow Variables) authored 2026-10-08**: Nuke-style graph-scope variables generalised to any knob type, animatable, set on the project or by a `ReverseFlowVariableSet` node and read in expressions as `rfv`. Order M70 → M71 → M72; no ordering against other backlog work was given, so they are unscheduled. Design: `DECISIONS/2026-10-08-reverse-flow-variables-design.md`.

- **Backlog order (2026-10-08, user):** M73 (wrap-up of M67 - Native Core Nodes) → M64 (re-evaluation only, stops for a go-ahead) → M27 → M22 → M31 → M69 → M24 → M44 → M55 and M46 together (evaluate Labelmaker-style node-graph info as part of node-graph polish) → M56 → M45. M25, M29 and M68 were not placed and stay unscheduled. **Deferred, do not start:** RFV (M70–M72), deep (M21, M51, M32), M16, and the 3D roadmap (M19, M20, M52–M54). Naming M31 here is the go-ahead its stub asked for. See `DECISIONS/2026-10-08-backlog-order-after-parcel-2.md`.
  **M74–M78 (native I/O and metadata) inserted 2026-10-08 (user):** they run right after the M64 re-evaluation, which was already in flight when they were planned, and ahead of M27 and the rest of the order above. See `DECISIONS/2026-10-08-m64-reeval-before-native-io.md`. **Read scope (2026-10-09, user):** the native Read covers every OIIO format, RAW is dropped, and the OFX Read container path is removed. M79 - Layered Document Readers (PSD, KRA, ORA, XCF) was added later and is unscheduled. **RAW returns (2026-10-09, user)** as M80 - Native RAW Support (rawspeed + darktable ports, CPU) and M81 - Raw GPU Kernels, after M79; see `DECISIONS/2026-10-09-raw-support-via-rawspeed-and-darktable-ports.md`. See `DECISIONS/2026-10-09-native-read-format-scope.md`. **M74 shipped 2026-10-09** (PR #44, squash `2cf878e7f`): native nodes carry per-frame metadata, bridged to OFX both ways. **M64 dropped for now (2026-10-08, user):** the host is too slow for its benchmarks.
  **M73 shipped 2026-10-08** (PR #43, squash `cb04295b9`): openfx-misc pinned to its merge commit `9fb0d0904`; every fork's SupportExt pin is now on openfx-supportext master; merged stack branches and `build/wt/` worktrees removed (M64 benchmarks saved to `build/bench/from-wt-m64/`).

# Board

| ID | Milestone | Status | File |
|----|-----------|--------|------|
| M0 | Fork And Cut Scope | done | [M0-fork-cut-scope.md](PLAN/MILESTONES/M0-fork-cut-scope.md) |
| M1 | Toolchain Baseline | done | [M1-toolchain-baseline.md](PLAN/MILESTONES/M1-toolchain-baseline.md) |
| M2 | Qt6 Migration | done | [M2-qt6-migration.md](PLAN/MILESTONES/M2-qt6-migration.md) |
| M7 | Local Incremental Builds | done | [M7-local-incremental-builds.md](PLAN/MILESTONES/M7-local-incremental-builds.md) |
| M8 | Branching And CI/CD | done | [M8-branching-and-cicd.md](PLAN/MILESTONES/M8-branching-and-cicd.md) |
| M9 | ~~Drop Vendored OFX~~ | cancelled | [M9-drop-vendored-ofx.md](PLAN/MILESTONES/M9-drop-vendored-ofx.md) |
| M10 | Clean-Sheet CI/CD | done | [M10-cicd-clean-sheet.md](PLAN/MILESTONES/M10-cicd-clean-sheet.md) |
| M3 | Dependency Modernization | done | [M3-dependency-modernization.md](PLAN/MILESTONES/M3-dependency-modernization.md) |
| M4 | CI/CD Rebuild | done | [M4-cicd-rebuild.md](PLAN/MILESTONES/M4-cicd-rebuild.md) |
| M12 | Documentation Cruft Removal | done | [M12-documentation-cruft-removal.md](PLAN/MILESTONES/M12-documentation-cruft-removal.md) |
| M13 | Full OFX Plugin Set | done | [M13-full-ofx-plugin-set.md](PLAN/MILESTONES/M13-full-ofx-plugin-set.md) |
| M5 | Test And Correctness Baseline | done | [M5-test-correctness-baseline.md](PLAN/MILESTONES/M5-test-correctness-baseline.md) |
| M6 | Documentation Pass | done | [M6-documentation-pass.md](PLAN/MILESTONES/M6-documentation-pass.md) |
| M15 | Release Packaging | done | [M15-release-packaging.md](PLAN/MILESTONES/M15-release-packaging.md) |
| M11 | OFX Plugin Integration Test | done | [M11-ofx-plugin-integration-test.md](PLAN/MILESTONES/M11-ofx-plugin-integration-test.md) |
| M14 | Documentation Orphan Branch | done | [M14-documentation-tree-and-doc-ci.md](PLAN/MILESTONES/M14-documentation-tree-and-doc-ci.md) |
| M17 | Typed Edges Framework | done | [M17-typed-edges-native-framework.md](PLAN/MILESTONES/M17-typed-edges-native-framework.md) |
| M26 | Test Teardown Flake Fix | done | [M26-test-fixture-teardown-flake.md](PLAN/MILESTONES/M26-test-fixture-teardown-flake.md) |
| M18 | Deep Compositing v1 | done | [M18-deep-compositing-v1.md](PLAN/MILESTONES/M18-deep-compositing-v1.md) |
| M23 | Relocatable Release Bundles | done | [M23-relocatable-release-bundles.md](PLAN/MILESTONES/M23-relocatable-release-bundles.md) |
| M39 | Layers Not Planes | done | [M39-layers-not-planes.md](PLAN/MILESTONES/M39-layers-not-planes.md) |
| M57 | Write Layer Regressions | done | [M57-write-node-plane-layer-regressions.md](PLAN/MILESTONES/M57-write-node-plane-layer-regressions.md) |
| M58 | Write Multiplane Pixel Bug | done | [M58-write-multiplane-pixel-data-bug.md](PLAN/MILESTONES/M58-write-multiplane-pixel-data-bug.md) |
| M59 | Pre-Commit Auto-Format | done | [M59-pre-commit-auto-format.md](PLAN/MILESTONES/M59-pre-commit-auto-format.md) |
| M38 | Layer Channel Widget | done | [M38-channel-layer-ui-organization.md](PLAN/MILESTONES/M38-channel-layer-ui-organization.md) |
| M43 | ~~Drop Premult Concept~~ | cancelled | [M43-drop-premult-concept.md](PLAN/MILESTONES/M43-drop-premult-concept.md) |
| M34 | Native Shuffle Node | done | [M34-new-native-shuffle-node.md](PLAN/MILESTONES/M34-new-native-shuffle-node.md) |
| M61 | Time-Varying Layers | done | [M61-layers-that-vary-with-time.md](PLAN/MILESTONES/M61-layers-that-vary-with-time.md) |
| M65 | RGBA Layers | done | [M65-rgba-rgb-alpha-layers.md](PLAN/MILESTONES/M65-rgba-rgb-alpha-layers.md) |
| M66 | Alpha-Only Plugin Support | done | [M66-plugin-alpha-only-moderate.md](PLAN/MILESTONES/M66-plugin-alpha-only-moderate.md) |
| M35 | ~~Remove Implicit Shuffle~~ | cancelled | [M35-remove-implicit-shuffle-elsewhere.md](PLAN/MILESTONES/M35-remove-implicit-shuffle-elsewhere.md) |
| M36 | ~~New Channel Affordance~~ | cancelled | [M36-new-channel-affordance.md](PLAN/MILESTONES/M36-new-channel-affordance.md) |
| M37 | Channel Management Nodes | done | [M37-channel-management-nodes.md](PLAN/MILESTONES/M37-channel-management-nodes.md) |
| M50 | Project OCIO Support | done | [M50-proper-ocio-support.md](PLAN/MILESTONES/M50-proper-ocio-support.md) |
| M60 | Deep Layers And Channels | done | [M60-deep-layers-and-channels.md](PLAN/MILESTONES/M60-deep-layers-and-channels.md) |
| M62 | Render Scaling Hotspots | done | [M62-render-scaling-hotspots.md](PLAN/MILESTONES/M62-render-scaling-hotspots.md) |
| M63 | Task-Graph Render Scheduler | done | [M63-task-graph-render-scheduler.md](PLAN/MILESTONES/M63-task-graph-render-scheduler.md) |
| M67 | Native Core Nodes | done | [M67-native-core-nodes.md](PLAN/MILESTONES/M67-native-core-nodes.md) |
| M28 | Free RAM Fix | done | [M28-free-ram-reads-memfree.md](PLAN/MILESTONES/M28-free-ram-reads-memfree.md) |
| M30 | Automated Beta Releases | done | [M30-automated-beta-releases.md](PLAN/MILESTONES/M30-automated-beta-releases.md) |
| M73 | Native Core Nodes Wrap-Up | done | [M73-native-core-nodes-wrap-up.md](PLAN/MILESTONES/M73-native-core-nodes-wrap-up.md) |
| M64 | Tiled Rendering | blocked | [M64-tiled-rendering.md](PLAN/MILESTONES/M64-tiled-rendering.md) |
| M74 | Native Metadata Core | done | [M74-native-metadata-core.md](PLAN/MILESTONES/M74-native-metadata-core.md) |
| M75 | Native Read | doing | [M75-native-read.md](PLAN/MILESTONES/M75-native-read.md) |
| M76 | Native Write | todo | [M76-native-write.md](PLAN/MILESTONES/M76-native-write.md) |
| M77 | Native Metadata Nodes | todo | [M77-native-metadata-nodes.md](PLAN/MILESTONES/M77-native-metadata-nodes.md) |
| M78 | Metadata In Expressions | todo | [M78-metadata-in-expressions.md](PLAN/MILESTONES/M78-metadata-in-expressions.md) |
| M79 | Layered Document Readers | todo | [M79-layered-document-readers.md](PLAN/MILESTONES/M79-layered-document-readers.md) |
| M80 | Native RAW Support | todo | [M80-native-raw-support.md](PLAN/MILESTONES/M80-native-raw-support.md) |
| M81 | Raw GPU Kernels | todo | [M81-raw-gpu-kernels.md](PLAN/MILESTONES/M81-raw-gpu-kernels.md) |
| M27 | Real Debug Build | todo | [M27-debug-build-defines-ndebug.md](PLAN/MILESTONES/M27-debug-build-defines-ndebug.md) |
| M22 | Missing Plugin Placeholder | todo | [M22-missing-plugin-placeholder.md](PLAN/MILESTONES/M22-missing-plugin-placeholder.md) |
| M31 | Architectural Cleanup | todo | [M31-architectural-cleanup.md](PLAN/MILESTONES/M31-architectural-cleanup.md) |
| M69 | Minor Cleanup | todo | [M69-minor-cleanup.md](PLAN/MILESTONES/M69-minor-cleanup.md) |
| M24 | Node Graph Category Colour | todo | [M24-node-graph-category-colour.md](PLAN/MILESTONES/M24-node-graph-category-colour.md) |
| M44 | Trackball Colour Editing | todo | [M44-trackball-colour-editing.md](PLAN/MILESTONES/M44-trackball-colour-editing.md) |
| M55 | Node Graph Polish | todo | [M55-polish-node-graph-interaction.md](PLAN/MILESTONES/M55-polish-node-graph-interaction.md) |
| M46 | Labelmaker Annotations | todo | [M46-labelmaker-annotations.md](PLAN/MILESTONES/M46-labelmaker-annotations.md) |
| M56 | Visual And Menu Polish | todo | [M56-polish-visual-and-menus.md](PLAN/MILESTONES/M56-polish-visual-and-menus.md) |
| M45 | Native Tab Menu | todo | [M45-tabtabtab-native-tab-menu.md](PLAN/MILESTONES/M45-tabtabtab-native-tab-menu.md) |
| M25 | GL Init FP Guard | todo | [M25-debug-fp-trap-gl-init.md](PLAN/MILESTONES/M25-debug-fp-trap-gl-init.md) |
| M29 | Independent Repository | todo | [M29-independent-repository.md](PLAN/MILESTONES/M29-independent-repository.md) |
| M68 | Headless GL EGL | todo | [M68-headless-gl-egl-backend.md](PLAN/MILESTONES/M68-headless-gl-egl-backend.md) |
| M19 | USD/Hydra Foundation | todo | [M19-usd-hydra-foundation.md](PLAN/MILESTONES/M19-usd-hydra-foundation.md) |
| M20 | 3D Node Vocabulary | todo | [M20-3d-node-vocabulary.md](PLAN/MILESTONES/M20-3d-node-vocabulary.md) |
| M52 | USD Layer-Stack Completion | todo | [M52-usd-layer-stack-completion.md](PLAN/MILESTONES/M52-usd-layer-stack-completion.md) |
| M53 | Projected Texture Painting | todo | [M53-projected-texture-painting.md](PLAN/MILESTONES/M53-projected-texture-painting.md) |
| M54 | SOPs-Style Geometry | todo | [M54-sops-style-geometry.md](PLAN/MILESTONES/M54-sops-style-geometry.md) |
| M21 | Deep Tier-2 And Bridges | todo | [M21-deep-tier2-and-bridges.md](PLAN/MILESTONES/M21-deep-tier2-and-bridges.md) |
| M51 | Deep Filtering OpenDCX | todo | [M51-deep-filtering-opendcx.md](PLAN/MILESTONES/M51-deep-filtering-opendcx.md) |
| M32 | Deep Sample Node | todo | [M32-deep-sample-node.md](PLAN/MILESTONES/M32-deep-sample-node.md) |
| M16 | Project Format Redesign | todo | [M16-project-file-format-redesign.md](PLAN/MILESTONES/M16-project-file-format-redesign.md) |
| M70 | RFV Declaration And Storage | todo | [M70-rfv-declaration-and-storage.md](PLAN/MILESTONES/M70-rfv-declaration-and-storage.md) |
| M71 | RFV Render Context | todo | [M71-rfv-render-context.md](PLAN/MILESTONES/M71-rfv-render-context.md) |
| M72 | RFV Integration And UAT | todo | [M72-rfv-integration-and-uat.md](PLAN/MILESTONES/M72-rfv-integration-and-uat.md) |

- 2026-09-18 — **M18's manual GUI checklist: all 6 items now confirmed**
  (re-run on a rebuilt AppImage at `94ceb9407` or later). Separately, the
  user judged M18.P2.T3's hover/tooltip probe itself the wrong design
  post-hoc (independent of whether it now works) — that is **not** a gate
  blocker, it's filed as new stub milestone M32 (see
  `PLAN/DECISIONS/2026-09-18-deepsample-node-replaces-hover-probe.md`).
  Also on 2026-09-18, four more items surfaced and were added to M18 as a
  new **Phase 18.4** (see M18's `## Decisions`): `DeepFromImage` creating
  zero-alpha samples it shouldn't, `DeepCrop`'s `Reformat` knob not
  refreshing the output format, and two M17-introduced node-graph visuals
  (dangling-input-pipe dot/diamond glyph, tinted rectangle behind a node's
  body) the user wants removed outright. A fifth task, `DeepReformat` (a new
  filterless format-reposition node with a centre option), was added
  mid-phase by explicit user request. **All five landed 2026-09-18; the
  gate re-check passed (216/216 ctest, Xvfb-verified glyph/fill removal) and
  M18 is `done`.** **Shipped 2026-09-18** — PR #24 squash-merged to `main`
  as `3e14c2a1e` after one Codex review round (10 findings: 5 fixed, 2
  false-positive, 3 documented tradeoffs — see M18's `## Decisions`); CI
  (`format`, `lint-ci`, `build-and-test`) green throughout.

# Open questions

- **M64 - Tiled Rendering:** dropped for now (user, 2026-10-08): this 4-core dev host is too slow for the Phase 64.7 re-evaluation benchmarks to be meaningful. Resume only on a faster host and with the user's go-ahead.
- **M55 - Node Graph Polish / M46 - Labelmaker Annotations:** are they one milestone or two? Decide after the evaluation described in the 2026-10-08 backlog-order decision.
