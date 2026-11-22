# M448 - USD Layer Break

A `USDLayerBreak` node lets an export hold only edits. Every layer upstream of a break is **context**: it still composes in the viewer, renders and every node's queries, but USDWrite leaves it out of what it writes. A lighting artist can read `landscape.usd`, add a break, prune the scratch light and add lights, and USDWrite then writes a `lighting.usd` holding only the `over` that deactivates the scratch light and the new lights. The pipeline layers that file over `landscape.usd` later. USDWrite gains a `Strip Layers Above Breaks` toggle, on by default, honoured in both Layered and Flattened modes. First member of F27 - USD Layer Authoring.

Context is a flag on each stack entry, not a marker in USD data. `SceneLayerEntry` (`Engine/Nodes/Scene/ScenePayloadImpl.h`) gains `bool context`. The break re-appends its input's entries with the flag set and adds no layer of its own. USDWrite's layered path (`writeFrameRange`, `appendLayerStack`, `layOutStack` in `Engine/Nodes/Scene/USDWrite.cpp`) skips context entries.

## Scope

**In:**
- The `USDLayerBreak` node (no knobs), with its `.reg.inc`, doc page and icon.
- The `context` flag on `SceneLayerEntry`, kept through `appendEntry`, USDMerge's `mergeStacks` and `reparentRoots`.
- USDWrite's `stripLayerBreaks` knob ("Strip Layers Above Breaks", default on) in both export modes.
- ctest coverage, the USDWrite doc update, and one Xvfb GUI script.

**Out:**
- Save paths, layer regions and per-layer metadata: M449 - USD Layer Save Paths.
- The export preview tree and pre-render error reporting: M450 - USDWrite Export Preview.
- Save-style choices beyond Layered/Flattened, and per-frame files or value clips: user scoped them out (2026-11-15), so no milestone owns them.
- Editing a file layer in place (Solaris Load Layer): no milestone.
- Marking context prims in the Scene Graph panel: no milestone.

**Edge cases:**
- A file read above the break (USDRead's sublayer) is omitted from the output entirely, not sublayered by path. The delta does not open standalone.
- USDMerge of a branch with a break and a branch without: only layers upstream of a break on their own path are context. All of the unbroken branch is written.
- One layer reached through both a broken and an unbroken path (one USDRead feeding both merge inputs): it is written. In `mergeStacks`, the kept entry is context only if every occurrence is.
- A USDReference after the break whose source input is all context: the referenced stack is still written as a sidecar, as layered mode does today. Only its read files stay external.
- Chained breaks: the second adds nothing new to strip beyond the layers between them, which it also marks as context. Cumulative by construction.
- A disabled or bypassed break passes its input through unchanged, so nothing is marked.
- Prims defined only above the break and edited after it are written as `over` specs. That is USD's normal behaviour for a layer that does not define them.
- Stage metadata (upAxis, metersPerUnit, defaultPrim, time range, render settings prim) is still authored on the root output from the composed payload's metadata, even when the layer that set it is context.
- Flattened mode with the toggle on composes a stage from only the non-context entries and flattens that. Reference arcs are baked in, and prims that only context defined come out as overs.
- The toggle off writes everything, exactly as today.

**Assumptions:**
- Unconfirmed: a break inside a referenced or grafted input stack (USDReference source, USDXformPrim graft wrappers from `reparentRoots`) is ignored: that stack is written whole. The user chose "write it as a sidecar" over "follow the input's breaks". Confirm at UAT whether a break under a USDXformPrim parent should strip.
- Composing a stage from a subset of entries for Flattened mode works with `SceneOps::compose` given a payload rebuilt from those entries; their dependencies keep referenced anonymous layers alive.

## Phase 448.1: Context flag and break node

- [ ] M448.P1.T1 — `context` flag on scene stack entries
  - files: `Engine/Nodes/Scene/ScenePayloadImpl.h`, `Engine/Nodes/Scene/SceneOps.h`/`.cpp` (`appendEntry`, `getLayers`, `reparentRoots`, the payload hash), `Engine/Nodes/Scene/USDMerge.cpp` (`mergeStacks`), `Tests/Scene/USDMergePrune_Test.cpp`
  - approach: add `bool context = false` to `SceneLayerEntry`, and fold it into the stack hash so a break invalidates downstream memos. Add `SceneOps::markContext(const ScenePayload&) → ScenePayload`, which rebuilds the chain with every entry flagged and keeps metadata, dependencies, links and live textures. In `mergeStacks`, the kept entry is context only if every occurrence of that layer across the inputs is. `reparentRoots` carries the flag on any entry it copies through. Graft wrappers are new layers authored by the reparenting node, so they are not context.
  - verify: unit tests. `markContext` flags every entry and changes the hash. A merge of a marked and an unmarked stack keeps the unmarked branch's entries unflagged. A layer shared by a marked and an unmarked input comes out unflagged. Existing USDMerge tests pass.
  - size: M

- [ ] M448.P1.T2 — `USDLayerBreak` node
  - files: `Engine/Nodes/Scene/USDLayerBreak.h`/`.cpp`/`.reg.inc`, `Engine/Nodes/Scene/docs/USDLayerBreak.md`, icon beside the other scene node icons, `Engine/CMakeLists.txt` if scene sources are listed, `Tests/Scene/USDLayerBreak_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: a `NativeEffectBase` scene node with one Scene input and no knobs, modelled on USDPrune's registration (`USDPrune.reg.inc`). Its output is `SceneOps::markContext(input)`. It authors no layer. The doc page explains context vs. written layers with the lighting example, and says the break does not change what the viewer shows.
  - verify: unit test. A Read → Prune → Break → Prune graph: the output payload's first two entries are context and the last is not, and the composed stage is identical to the same graph without the break.
  - size: M

## Phase 448.2: USDWrite strips context

- [ ] M448.P2.T1 — Layered mode skips context entries
  - files: `Engine/Nodes/Scene/USDWrite.h`/`.cpp` (`initializeKnobs`, `writeFrameRange`), `Engine/Nodes/Scene/docs/USDWrite.md`
  - approach: add `stripLayerBreaks` (`KnobBool`, label "Strip Layers Above Breaks", default true, tooltip saying that context layers above a USDLayerBreak are left out). In the layered branch of `writeFrameRange`, skip entries with `context` set when building `items` for the root stack, so their sublayered read files go too. Referenced stacks (`stacks[i]`, i > 0) are written as today. Sidecar numbering and stale-sidecar removal follow the reduced layout. Root metadata still comes from the payload (`authorStageMetadata`).
  - verify: `Tests/Scene/USDWrite_Test.cpp` gains `LayerBreakWritesOnlyEditsAfterTheBreak`. Read the M52 fixture → Break → USDPrune of one prim → USDLight → USDWrite. The written file has no sublayers. It holds an `over` with `active = false` for the pruned prim and a `def` for the light, and nothing from the fixture. Layering it over the fixture with a fresh stage reproduces the graph's composed stage. With the toggle off, the output matches today's layered output.
  - size: M

- [ ] M448.P2.T2 — Flattened mode honours the break
  - files: `Engine/Nodes/Scene/USDWrite.cpp` (flattened branch of `writeFrameRange`), `Tests/Scene/USDWrite_Test.cpp`
  - approach: with `stripLayerBreaks` on, rebuild a payload from the non-context entries, compose it with `SceneOps::compose`, and `Flatten(false)`. Keep the `keep` set of used file layers as today, so files nodes still read are not removed. With the toggle off, or with no context entries, the current path is unchanged.
  - verify: test `LayerBreakFlattenedHoldsOnlyEdits`, with the same graph as P2.T1 in flattened mode. The output defines the light and overs the pruned prim, and holds no fixture prim spec beyond the overs.
  - size: M

- [ ] M448.P2.T3 — Merge and reference cases end to end
  - files: `Tests/Scene/USDWrite_Test.cpp`
  - approach: three tests. (a) USDMerge of [Read A → Break → edit] and [Read B → edit]: A is absent, while B's file is sublayered and its edit written. (b) One Read feeding both merge inputs, one through a break: the read file is still sublayered. (c) Read → Break → USDReference whose source input is a second Read → edit chain with no break: the referenced stack is written as a sidecar with its read file external, and the root output holds the reference arc.
  - verify: the three tests pass, along with every existing `USDWrite_Test` case.
  - size: M

- [ ] M448.P2.T4 — GUI script for the break
  - files: `Tests/gui/m448_layer_break.py`, the GUI test registration beside `Tests/gui/m52_layer_stack.py`
  - approach: under Xvfb, build Read(fixture) → USDLayerBreak → USDPrune → USDWrite from Python. Check that the viewer's scene still shows the fixture prims. Render USDWrite and check the written file's prim specs from Python by reading it back with a USDRead.
  - verify: the script passes in the remote `--gui` gate.
  - size: M

**Verification gate:** CI green, including the new USDLayerBreak, USDMerge and USDWrite tests and `m448_layer_break.py` under `--gui`. With the toggle on, layered and flattened exports after a break hold only post-break edits; with it off, they match pre-milestone output. The user-facing check (lighting-over-landscape delta in a real session) is collected at F27 - USD Layer Authoring's UAT.

## Decisions

- 2026-11-15 — Read files above a break are omitted, not sublayered (user, grill): a pure delta that layers over any version of the base, as Solaris does.
- 2026-11-15 — Per-layer context marking through merges (user, grill): only layers upstream of a break on their own path are context; a layer also reachable by an unbroken path is written, since dropping it would lose data that branch relies on.
- 2026-11-15 — A reference made after the break writes its referenced stack as a sidecar even if that input is all context (user, grill): the arc is a post-break edit and its target must exist on disk.
- 2026-11-15 — `Strip Layers Above Breaks` toggle on USDWrite, default on, in both modes; Flattened writes only post-break layers flattened (user, grill).
- 2026-11-15 — Context is a `SceneLayerEntry` flag, not USD customData (planner): it keeps USD data clean (`ExportsCarryNoNatronCustomData`) and survives `mergeStacks` deduplication with an explicit rule.
- 2026-11-15 — The export preview the user asked for is its own milestone, M450 - USDWrite Export Preview, after M449 - USD Layer Save Paths, so it shows save-path files too (planner, user did not object).
- 2026-11-22 — Review round 1 (#337): a stripped flattened write flattens only the post-break layer stack (UsdFlattenLayerStack via flattenRun) and bakes just the scene nodes' own reference/payload arcs, so variant selections, inherits/references to context prims, list ops and deactivations survive as authored (replaces the Flatten-based path and authorDeactivations; settles the open question on lost variant selections). Limits: arcs inside variant bodies aren't baked; arc offsets in read files at another time-codes-per-second aren't corrected. Knob renamed stripLayersAboveBreaks; layer keys use a flag-free content hash (23027876e).
