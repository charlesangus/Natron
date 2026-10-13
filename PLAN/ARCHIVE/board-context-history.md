# Board context history

Moved verbatim from the board's `# Context and constraints` (and a stray paragraph after the `# Board` table) by `/cat-update`. Historical narrative; not read in normal operation.

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

- **M16 is authored but deferred** (2026-09-05): the `.ntp` format redesign is
  future work — do not start or elaborate it without an explicit user
  go-ahead. **Go-ahead given 2026-10-10 (user):** picked up on a separate host. Elaborated
  2026-10-10 into **M93 - Serialization Boundary** (plain-data `Serialization/` library, no
  format change, golden-XML byte-for-byte harness) followed by M16 (`.ntpz` bundle, clean
  break with no XML import, sibling-relative connections). M93 runs now and rebases onto
  M76 - Native Write when it merges; **M92 - Linked Nodes should build on M93's plain
  structs**. See `DECISIONS/2026-10-10-ntpz-clean-break-sibling-connections.md`. Design and evidence live in
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
  **M82–M87 (GPU rendering) added 2026-10-09 via the inbox (user-requested):** M82 - Render Cost Profiling measures first; M83 - GPU Compute Backend Spike picks one engine-wide backend (Vulkan compute + Slang is the lean, pending the user); M84 - GPU Placement And Residency, M85 - Native GPU Kernels, M86 - OFX 1.5 GPU Suites and M87 - Point-Op Kernel Fusion follow. They sit after M80, with M81 - Raw GPU Kernels moved behind M84 so raw kernels feed a resident region. M68 - Headless GL EGL stays deferred and is re-scoped once M83 lands. See `DECISIONS/2026-10-09-one-engine-gpu-backend.md`.
  **M102–M106 (live sessions + graph merge) added 2026-10-11 via the inbox (user-requested), renumbered from M95–M99 on 2026-10-12 because those IDs were already taken:** M102 - Graph Edit Operations is the shared foundation (persistent node ID, plain-data edit operations). M103 - Graph Diff And Merge and M104 - Merge Resolution UI deliver three-way template merge. M105 - Live Session Core and M106 - Live Session Presence And Undo deliver live multi-user editing. All of it runs after M16 - Project Format Redesign. See `DECISIONS/2026-10-11-live-sessions-and-graph-merge.md`.
  **M100 - Content Node Hashing added 2026-10-11 (user):** content (Merkle) node hashes replace `knobsAge`; see `DECISIONS/2026-10-11-content-node-hashing.md`. **M101 - Clean-Sheet Merge Node added 2026-10-12 (user):** a new native `Merge` after M84 - GPU Placement And Residency replaces the openfx-misc Merge port with no compatibility, and M85 - Native GPU Kernels no longer ports Merge; see `DECISIONS/2026-10-12-clean-sheet-merge.md`.
  **Native nodes drop OFX legacy (2026-10-12, user):** the README rule that native nodes copy their OFX plug-in's knobs, defaults and maths is retired. Native nodes take plain IDs (`Grade`, not `net.sf.openfx.GradePlugin` or `fr.natron.Grade`) at major 1, and sibling plug-ins become one node. M111 - Native Node Identity Cleanup does IDs, siblings and dead code; M112 - Native Node Behaviour Fixes does the maths; M95 - Knob Cleanup And Keyer Redesign also takes ChromaKeyer. See `DECISIONS/2026-10-12-native-nodes-drop-ofx-legacy.md`.
  **M84 shipped 2026-10-12** (PR #54, squash `6fcf9c4fa`): Vulkan compute + Slang is in the main build, the default GPU mode is Auto, Grade and Blur (FIR Gaussian) run on the GPU, and resident regions pipeline their strips on the transfer queue. Hand-offs are in M85 - Native GPU Kernels and M86 - OFX 1.5 GPU Suites.
  **M85 split (user, 2026-10-12):** M85 - Native GPU Kernels covers the kernel description, the per-pixel mask/mix/premult wrapping (`pixelwrap.slang`) and the node ports (Grade and Blur complete, point ops, keyers, morphology, Transform). Split off from it: M107 - GPU Viewer Hand-off, M108 - Retire Engine GL Host Path (separate from M86, runs before it), M109 - GPU Region Engine Hardening and M110 - Remaining Native GPU Ports.
  **M73 shipped 2026-10-08** (PR #43, squash `cb04295b9`): openfx-misc pinned to its merge commit `9fb0d0904`; every fork's SupportExt pin is now on openfx-supportext master; merged stack branches and `build/wt/` worktrees removed (M64 benchmarks saved to `build/bench/from-wt-m64/`).

  **M76 shipped 2026-10-12** (PR #52, squash `ca130dcf0`): one native OIIO Write for every writable format; `WriteNode`, the OFX writers and the IO meta-node plumbing are gone. A Write passes its input through as the file would hold it and writes only on an explicit render; Reload buttons on the Read and Write. Hash-gated read back moved to M97 - Value-Based Node Hashing (stub).

  **M24 shipped 2026-10-10** (PR #50, squash `a50bef552`). Nodes take their body colour from a category, and the user colour is a ring outside the node. Category and edge-kind colours are set in Preferences and applied on Save. Labels pick black or white text for contrast, and Dots get a settings panel.

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
