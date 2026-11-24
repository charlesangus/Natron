# M578 - Scene Diff Inspector And Writer

Shows and publishes the difference between two scenes. A `USDDiffWrite` node takes a Base and a Layer scene input and writes one USD layer holding only what Layer changes over Base. Laid over Base, that layer reproduces Layer; Gaffer's USDLayerWriter has the same semantics. An inspector compare mode in the Scene Graph panel shows one prim of the current node beside a prim of a second node, row by row, with the differences marked. Both use one diff core in `Engine/Nodes/Scene/SceneOps.{h,cpp}`. The writer follows USDWrite (`Engine/Nodes/Scene/USDWrite.{h,cpp}`, `writeFrameRange`, its frame range, asset path and live texture handling, doc `Engine/Nodes/Scene/docs/USDWrite.md`). New prims are written with the composed-definition helpers the inspector's Layer tab uses (`SceneOps::beginPrimDefinition`, `flattenPropertyInto`). The compare mode extends `Gui/ScenePrimInspector.{h,cpp}`. Its second stage is read by a `SceneNodeStageSource` (`Gui/SceneNodeStageSource.h`) with the shown stage's load rules (`requestLoadRules`). Two related features are different things. M448 - USD Layer Break makes a delta structurally, by leaving out layers above a break, while this compares content and works on any two scenes. M103 - Graph Diff And Merge diffs node graphs, not USD scenes.

## Scope

**In:**
- Diff core: per prim, presence on each side, type, specifier, active, kind and other prim metadata, and every property: added, removed, or changed in default value, time samples over a frame range, interpolation, relationship targets or connections.
- `USDDiffWrite`, with its `.reg.inc`, the USD I/O toolbar submenu, an icon, `docs/USDDiffWrite.md` and tests. Its controls mirror Gaffer's USDLayerWriter, and USDWrite's where a Natron writer needs them:
  - base: the Base input. In.
  - layer: the Layer input. In.
  - fileName: File, a `.usda`/`.usdc`/`.usd` path. In.
  - out: the output, a pass-through of Layer, as USDWrite passes its input. In.
  - preTasks, postTasks, task and dispatcher.* (batchSize, immediate, tractor.*): Out. Natron's render queue, Render button and `NatronRenderer -w` run output nodes, as for USDWrite.
  - user: Out. Natron's user knobs exist on every node already.
  - Frame Range and First Frame/Last Frame (USDWrite's): In, as the times compared and written.
  - Asset Paths (USDWrite's): In, for asset-valued attributes in the delta.
  - Export Live Textures (USDWrite's): In, for `natron://` textures the delta carries.
- An inspector compare mode: a Compare To node chooser and a Compare Path field. Attributes, Relationships and Metadata get A and B value columns, differing rows are marked, and a Differences Only toggle is added.
- A params→knob audit task against USDLayerWriter and USDWrite.

**Out:**
- Writing arcs (references, payloads, variants) into the delta: no milestone. The delta holds composed values; see Edge cases.
- A layered delta that keeps file sublayers, as USDWrite's Layered mode does: no milestone.
- Comparing the Composition, Opinions, Layer, Time Samples and Layer Stack tabs: no milestone. They show A only in compare mode.
- Diffing node graphs: M103 - Graph Diff And Merge.
- Three-way merge of scenes: no milestone.

**Edge cases:**
- Prim only in Layer: written as `def` with its type, its composed properties and metadata, and its children, flattened.
- Prim only in Base: written as `over` with `active = false`, as USDPrune's default Deactivate does.
- Property only in Base: an attribute gets a value block, and a relationship gets an explicit empty target list.
- Prim metadata only in Base: USD cannot remove it with a stronger opinion, so it is listed in the node's warning ("<field> on <prim> cannot be removed by a layer").
- A change inside an instance (instance proxy contents differ): instance proxies cannot be authored, so the delta authors `instanceable = false` on the instance and the differing descendants under it, and warns naming the instance.
- Both sides instance the same prototype unchanged: no opinion is written.
- Time samples: compared at the union of both sides' sample times within the range, plus the range ends. A property that differs only outside the range is not written. A property animated on one side and static on the other is written as samples.
- Arcs in Layer that Base lacks (a new reference or variant selection) arrive as composed values, so the delta repeats the referenced data instead of the arc. This is documented in the node doc.
- Child order: a differing order of children is written as `primOrder` on the parent.
- Identical inputs: an empty layer with stage metadata only, plus a warning ("Base and Layer are the same").
- Stage metadata (up axis, meters per unit, time codes per second, default prim, render settings prim): written from Layer when it differs from Base, else left out.
- Payloads: like USDWrite, the writer composes both inputs with every payload loaded (`SceneLoadSet::everything()`), since a render loads everything.
- Cancel: the render's abort flag is checked per prim, and nothing is written when aborted, so the old file stays.
- A file the inputs read being overwritten: refused with an error, as USDWrite refuses it.
- Compare mode, B lacks the prim: the B column reads "no prim" and every row is marked.
- Compare mode, B is a node no viewer shows: read into the panel's own stage with A's load rules (`requestLoadRules`), so the same payloads are loaded on both sides and none is loaded without the user having loaded it for A.
- Compare mode, B's node is deleted or stops producing a scene: compare mode turns off with a status message.
- Compare mode at the current frame only. A change of frame rereads both sides.

**Assumptions:**
- Composing the two inputs as two `SceneQueryPool` leases on one thread works (each lease holds its own read scope).
- `beginPrimDefinition`/`flattenPropertyInto` can write a whole subtree when called per descendant; if not, T2 adds a subtree variant.

## Phase 578.1: Diff core

- [ ] M578.P1.T1 — Diff one prim against another
  - files: `Engine/Nodes/Scene/SceneOps.{h,cpp}` (`ScenePrimDiff`, `diffPrim`), `Tests/Scene/SceneDiff_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: `diffPrim(stageA, pathA, stageB, pathB, times, ScenePrimDiff*)` compares presence, type, specifier, active, metadata fields and each property's default, samples at `times` (union with the authored sample times inside the range), interpolation, relationship targets and connections. The result is a pxr-free list of `{name, kind: added|removed|changed, field}` entries, so the GUI can use it too.
  - verify: gtest on in-memory stages: an attribute changed in default only, in one sample only, and outside the range (not reported); an added and a removed property; a retargeted relationship; a type change; identical prims give an empty diff.
  - size: M

- [ ] M578.P1.T2 — Diff two stages and author the delta layer
  - files: `Engine/Nodes/Scene/SceneOps.{h,cpp}` (`diffStages`, `authorDelta`), `Tests/Scene/SceneDiff_Test.cpp`
  - approach: walk both composed stages together in path order (instance proxies included, abort checked per prim) using `diffPrim`. `authorDelta(baseStage, layerStage, diffs, times, SdfLayer*, warnings*)` writes the Edge-cases opinions: `def` subtrees for Layer-only prims, `active = false` for Base-only prims, blocks for removed attributes, empty targets for removed relationships, `primOrder`, de-instancing for changes inside instances, and differing stage metadata.
  - verify: gtest round trip on five fixtures (added prim tree, removed prim, changed and animated attributes, a change inside an instance, reordered children): compose Base's layers plus the delta, `diffStages` against Layer gives no differences over the range; the removed-metadata case returns its warning.
  - size: L

## Phase 578.2: USDDiffWrite

- [ ] M578.P2.T1 — `USDDiffWrite` node
  - files: `Engine/Nodes/Scene/USDDiffWrite.{h,cpp,reg.inc}` (new), `Engine/CMakeLists.txt`, `Gui/Resources/Images/` (icon), `Tests/Scene/USDDiffWrite_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: an output node like USDWrite with two scene inputs, Base (0) and Layer (1), and output = Layer. Knobs `file` ("File"), `frameRange` ("Frame Range"), `firstFrame`, `lastFrame`, `assetPaths` ("Asset Paths") and `exportLiveTextures` ("Export Live Textures") reuse USDWrite's helpers for the range, asset path rewriting, live texture export and reload-in-place. Writing borrows both inputs with `SceneLoadSet::everything()`, runs `diffStages` and `authorDelta` into an anonymous layer, and exports it. Errors and warnings are set as USDWrite sets them. It registers in the USD I/O submenu (its `PLUGIN_GROUP_USD_*` macro from M520 - USD Toolbar Category And Submenus).
  - verify: gtest: USDRead(model) as Base and the same read through USDSetAttribute and USDPrune as Layer write a file holding only the attribute `over` and the `active = false`; sublayering it over the model file reproduces Layer; an empty File errors; an abort mid-write leaves an existing file untouched.
  - size: L

- [ ] M578.P2.T2 — Doc page and GUI check
  - files: `Engine/Nodes/Scene/docs/USDDiffWrite.md` (new), `Tests/gui/scene_diff_write.py` (new)
  - approach: write the doc in USDWrite.md's style, covering inputs, every knob, the delta rules from Edge cases (deactivated removals, blocks, flattened arcs, de-instancing) and errors, with no reference-app names. The script builds the graph, renders the node from the GUI and reads the written file back with a USDRead.
  - verify: the Xvfb script at 1920x1080 renders, the read-back stage has the expected `over`, and the node shows the "same inputs" warning when both inputs are connected to one node.
  - size: M

- [ ] M578.P2.T3 — Params→knob audit
  - files: `Engine/Nodes/Scene/docs/USDDiffWrite.md`, `Engine/Nodes/Scene/USDDiffWrite.cpp`
  - approach: check every In and Out control in `## Scope` against the node's knobs, inputs and doc. Labels must be the readable script names. Fix any gap, and record the audit table in this milestone's `## Decisions`.
  - verify: the Decisions entry lists each USDLayerWriter and USDWrite control with its knob or reason, and nothing In is missing.
  - size: M

## Phase 578.3: Inspector compare mode

- [ ] M578.P3.T1 — Compare To controls and the B stage
  - files: `Gui/ScenePrimInspector.{h,cpp}`, `Gui/SceneGraphPanel.cpp`, `Tests/gui/scene_inspector_compare.py` (new)
  - approach: a Compare To row above the tabs (`sceneGraphInspectorCompareNode`, a scene-node chooser with None; `sceneGraphInspectorComparePath`, empty meaning the selected path). With a node chosen, the panel creates a `SceneNodeStageSource` for it, hands it the shown stage's load rules through `requestLoadRules`, and keeps it in step as the user loads or unloads on A. The inspector read then gets both sources. The other compare behaviour in Edge cases applies.
  - verify: Xvfb script: choosing a second node creates its source, and loading a payload on A loads it on B (checked through `natronSceneGraph*` properties); deleting B turns compare mode off with the status message.
  - size: M

- [ ] M578.P3.T2 — A and B columns with differences marked
  - files: `Gui/ScenePrimInspector.cpp`, `Tests/gui/scene_inspector_compare.py`
  - approach: in compare mode, Attributes, Relationships and Metadata read both prims (one lock call per row per side) and show Value A and Value B, keeping Authored By for each. `diffPrim` at the current frame marks the differing rows (a bar in the first column, the row tinted). A Differences Only toggle (`sceneGraphInspectorDifferencesOnly`) hides equal rows. The other tabs show A with a note that they don't compare.
  - verify: Xvfb script: comparing USDSphere1 with USDSphere1 → USDSetAttribute(radius) marks only `radius`, and Differences Only leaves one row; a B prim missing reads "no prim" on every row; a screenshot of the compare view at 1920x1080 goes to the user.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `SceneDiff_Test` and `USDDiffWrite_Test` pass; `Tests/gui/scene_diff_write.py` and `Tests/gui/scene_inspector_compare.py` pass under Xvfb at 1920x1080; the user OKs the compare-view screenshot.

## Decisions

- 2026-11-24 — **Both (user):** an inspector mode that compares one prim across two nodes' stages, and a node that writes only the delta between two input stages as a layer, with USDLayerWriter semantics.
- 2026-11-24 — One milestone in three phases, sharing one diff core. It depends on M571 - Prim Inspector Value Sources so the inspector rewrite lands once, and on M520 for the new node's submenu (planner).
