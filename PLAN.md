---
title: Linux-Only Qt6 Foundation Plan
status: running
current: null
pm_heartbeat: 2026-09-19T00:48:06-04:00
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
  output.

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
| M3 | Dependency modernization | done | [M3-dependency-modernization.md](PLAN/MILESTONES/M3-dependency-modernization.md) |
| M4 | CI/CD rebuild | done | [M4-cicd-rebuild.md](PLAN/MILESTONES/M4-cicd-rebuild.md) |
| M12 | Documentation cruft removal | done | [M12-documentation-cruft-removal.md](PLAN/MILESTONES/M12-documentation-cruft-removal.md) |
| M13 | Build the full upstream OFX plugin set | done | [M13-full-ofx-plugin-set.md](PLAN/MILESTONES/M13-full-ofx-plugin-set.md) |
| M5 | Test & correctness baseline | done | [M5-test-correctness-baseline.md](PLAN/MILESTONES/M5-test-correctness-baseline.md) |
| M6 | Documentation pass | done | [M6-documentation-pass.md](PLAN/MILESTONES/M6-documentation-pass.md) |
| M15 | Release packaging: tarball and AppImage | done | [M15-release-packaging.md](PLAN/MILESTONES/M15-release-packaging.md) |
| M11 | OFX plugin integration test (post-release hardening) | done | [M11-ofx-plugin-integration-test.md](PLAN/MILESTONES/M11-ofx-plugin-integration-test.md) |
| M14 | Documentation tree → orphan branch | done | [M14-documentation-tree-and-doc-ci.md](PLAN/MILESTONES/M14-documentation-tree-and-doc-ci.md) |
| M17 | Typed graph edges and native node framework | done | [M17-typed-edges-native-framework.md](PLAN/MILESTONES/M17-typed-edges-native-framework.md) |
| M26 | Fix the shared test-fixture teardown flake | done | [M26-test-fixture-teardown-flake.md](PLAN/MILESTONES/M26-test-fixture-teardown-flake.md) |
| M18 | Deep compositing v1 | done | [M18-deep-compositing-v1.md](PLAN/MILESTONES/M18-deep-compositing-v1.md) |
| M23 | Make release bundles actually relocatable | done | [M23-relocatable-release-bundles.md](PLAN/MILESTONES/M23-relocatable-release-bundles.md) |
| M39 | Adopt "layer" terminology instead of "planes" | done | [M39-layers-not-planes.md](PLAN/MILESTONES/M39-layers-not-planes.md) |
| M57 | Fix Write node plane/layer regressions found while testing M39 | todo | [M57-write-node-plane-layer-regressions.md](PLAN/MILESTONES/M57-write-node-plane-layer-regressions.md) |
| M35 | Remove implicit output-plane shuffling from non-Shuffle nodes | todo | [M35-remove-implicit-shuffle-elsewhere.md](PLAN/MILESTONES/M35-remove-implicit-shuffle-elsewhere.md) |
| M38 | Improve channel/layer information organization in the node UI | todo | [M38-channel-layer-ui-organization.md](PLAN/MILESTONES/M38-channel-layer-ui-organization.md) |
| M34 | New native Shuffle node | todo | [M34-new-native-shuffle-node.md](PLAN/MILESTONES/M34-new-native-shuffle-node.md) |
| M36 | Add "new channel/layer" affordance wherever a node outputs channels | todo | [M36-new-channel-affordance.md](PLAN/MILESTONES/M36-new-channel-affordance.md) |
| M37 | Channel/layer management nodes | todo | [M37-channel-management-nodes.md](PLAN/MILESTONES/M37-channel-management-nodes.md) |
| M50 | Proper OCIO support as a project property | todo | [M50-proper-ocio-support.md](PLAN/MILESTONES/M50-proper-ocio-support.md) |
| M43 | Drop the premultiplied/unpremultiplied concept | todo | [M43-drop-premult-concept.md](PLAN/MILESTONES/M43-drop-premult-concept.md) |
| M25 | Guard the GL init path against the debug FP traps | todo | [M25-debug-fp-trap-gl-init.md](PLAN/MILESTONES/M25-debug-fp-trap-gl-init.md) |
| M27 | Make the debug build a debug build again | todo | [M27-debug-build-defines-ndebug.md](PLAN/MILESTONES/M27-debug-build-defines-ndebug.md) |
| M28 | Stop treating page cache as memory pressure | todo | [M28-free-ram-reads-memfree.md](PLAN/MILESTONES/M28-free-ram-reads-memfree.md) |
| M22 | Lossless project round-trip with missing plugins | todo | [M22-missing-plugin-placeholder.md](PLAN/MILESTONES/M22-missing-plugin-placeholder.md) |
| M29 | Break the link to upstream — an independent repository | todo | [M29-independent-repository.md](PLAN/MILESTONES/M29-independent-repository.md) |
| M30 | Full release + AppImage on every merge, auto-versioned betas from 0.1.0-beta1 | todo | [M30-automated-beta-releases.md](PLAN/MILESTONES/M30-automated-beta-releases.md) |
| M24 | Node graph aesthetics: category colour and user colour | todo | [M24-node-graph-category-colour.md](PLAN/MILESTONES/M24-node-graph-category-colour.md) |
| M44 | Trackball-style colour editing | todo | [M44-trackball-colour-editing.md](PLAN/MILESTONES/M44-trackball-colour-editing.md) |
| M55 | Polish — node graph interaction | todo | [M55-polish-node-graph-interaction.md](PLAN/MILESTONES/M55-polish-node-graph-interaction.md) |
| M56 | Polish — visual and menus | todo | [M56-polish-visual-and-menus.md](PLAN/MILESTONES/M56-polish-visual-and-menus.md) |
| M19 | USD/Hydra foundation: ScenePayload, ReadScene, Viewport3D | todo | [M19-usd-hydra-foundation.md](PLAN/MILESTONES/M19-usd-hydra-foundation.md) |
| M20 | 3D node vocabulary and HydraRender | todo | [M20-3d-node-vocabulary.md](PLAN/MILESTONES/M20-3d-node-vocabulary.md) |
| M52 | USD layer-stack completion: schema-driven edits, path expressions, composition nodes | todo | [M52-usd-layer-stack-completion.md](PLAN/MILESTONES/M52-usd-layer-stack-completion.md) |
| M53 | Projected texture painting: UV bake, ProjectTexture, live textures | todo | [M53-projected-texture-painting.md](PLAN/MILESTONES/M53-projected-texture-painting.md) |
| M54 | SOPs-style geometry: GeoDetail data kind, bridges, operators, point editing | todo | [M54-sops-style-geometry.md](PLAN/MILESTONES/M54-sops-style-geometry.md) |
| M21 | Deep tier-2 nodes and deep/3D bridges | todo | [M21-deep-tier2-and-bridges.md](PLAN/MILESTONES/M21-deep-tier2-and-bridges.md) |
| M51 | Deep filtering nodes (OpenDCX integration) | todo | [M51-deep-filtering-opendcx.md](PLAN/MILESTONES/M51-deep-filtering-opendcx.md) |
| M32 | Deep sample inspector node (replaces the hover probe) | todo | [M32-deep-sample-node.md](PLAN/MILESTONES/M32-deep-sample-node.md) |
| M45 | Port tabtabtab-nuke as the native tab menu | todo | [M45-tabtabtab-native-tab-menu.md](PLAN/MILESTONES/M45-tabtabtab-native-tab-menu.md) |
| M46 | Port Labelmaker as a native node graph annotation feature | todo | [M46-labelmaker-annotations.md](PLAN/MILESTONES/M46-labelmaker-annotations.md) |
| M16 | Project file format redesign (.ntp successor) | todo | [M16-project-file-format-redesign.md](PLAN/MILESTONES/M16-project-file-format-redesign.md) |
| M31 | Architectural cleanup (deferred; render-root ownership by composition) | todo | [M31-architectural-cleanup.md](PLAN/MILESTONES/M31-architectural-cleanup.md) |

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

- **USD Python inside Natron's Python?** M19.P1.T1 builds USD with Python
  OFF (Shiboken6/PySide6 vs pxr coexistence). A Solaris-style "Python over
  the stage" node would cover the LOPs long tail cheaply but needs pxr
  bindings loaded into Natron's interpreter. M52.P3.T3's `SetAttribute3D`
  is the no-Python substitute for now. Decide whether to spike pxr-in-Natron
  coexistence after M52 ships, or leave it out of scope.
