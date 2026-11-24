# M571 - Prim Inspector Value Sources

Answers "why is this value what it is" in the Scene Graph panel's prim inspector. Each attribute's value is coloured by where it comes from. Computed rows show what USD derives (world transform, bounds, resolved visibility, bound material). A Time Samples tab and a Layer Stack tab are added. M333 - Prim Inspector built `Gui/ScenePrimInspector.{h,cpp}`: `ScenePrimInspectorTab` pages read on a worker through `ScenePrimInspectorRead::readStage`, in short lock calls. Its tabs are Attributes (Attribute, Type, Value, Source, Authored By; ~:1477), Relationships, Metadata, Composition, Opinions and Layer. The Source column is text only. `sourceLabel(SceneOps::SceneValueSource)` (~:180) names Fallback, Default, Time Samples, Value Clips and Spline, from `SceneOps::readAttribute` (`Engine/Nodes/Scene/SceneOps.{h,cpp}`, `SceneValueSource` ~:851), which reads `UsdAttribute::GetResolveInfo`. The design reference is Solaris's Scene Graph Details pane (`.local/docs/houdini/ref/panes/scenegraphdetails.md`), whose colours follow usdview's.

## Scope

**In:**
- Value text coloured by source, with a legend line under the Attributes table. Each colour is In:
  - Not set: grey.
  - Fallback (schema default): orange.
  - Set (authored default): blue.
  - Time sampled: green.
  - Value clip: violet.
  - Computed: indigo.
  - Relationships: red.
  - Spline: teal. USD's spline animation is a source Solaris predates, so it gets its own colour.
  - Exact RGB values are picked for contrast on the Natron dark palette and named in one table in the code.
- Computed rows at the top of Attributes, read-only: World Transform, Local Transform, World Bound, Local Bound, Computed Visibility, Computed Purpose, Bound Material (all purposes) and Bound Material (preview).
- A Decompose Transforms toggle that shows the transform rows as translate, rotate and scale.
- A Time Samples tab for the selected attribute.
- The Metadata tab following the selected attribute or relationship, falling back to the prim's metadata.
- A Layer Stack tab: the stage's layer stack plus the layers the prim's arcs bring in.
- Audit of Solaris's Scene Graph Details toolbar and tabs (each In or Out):
  - Tree View (attributes as children of prims) and List View (prims as rows, attributes as columns): Out. The inspector shows one prim, and per-element and many-prim tables are M337 - Geometry Spreadsheet's.
  - Side-by-side and Top-bottom split: Out. The panel's existing tree-over-inspector layout is kept.
  - Decompose Transforms: In.
  - Filter (show transforms as T, R and S): In, as the Decompose toggle. A name filter is not part of it.
  - Value tab (the whole value, element rows, copy): Out. The existing double-click full-value window shows the whole value, and per-element rows are M337's.
  - Time Samples tab, with Copy CSV: In.
  - Metadata tab, for the selected property: In.
  - Editor Nodes tab: Out. The Opinions tab already lists the authoring node of every spec, strongest first, and opens it on double-click.
  - Layer Stack tab: In.
  - Composition tab: kept as M333 built it. The audit adds only the source colour on its unloaded-payload rows (grey, "not loaded").

**Out:**
- Muting layers from the Layer Stack tab: M580 - Scene Load Masks.
- Editing values from the inspector: M581 - Edit Scope Design decides where such edits go.
- Comparing a prim across two nodes: M578 - Scene Diff Inspector And Writer.
- Authoring value clips: M579 - USD Value Clip Sequences. This milestone only shows them.

**Edge cases:**
- An attribute with both a default and time samples resolves to samples: green. A value-clip attribute outside its clip's active range resolves to its default or fallback, and is coloured by what `GetResolveInfo` reports at the frame.
- A blocked attribute (`None` authored) shows as Not set, grey, with "blocked" in Source.
- Computed rows on a prim that is not xformable or boundable: those rows are left out, not shown empty.
- World Bound on a prim with unloaded payloads below it counts the loaded prims only, says "loaded prims only" in Source, and never loads anything.
- Computed rows read after the authored rows, one lock call each, so a slow bound shows last and a selection change cancels it between rows. If a World Bound on the Island flat file's top prim blows the M333 stall budget, the bound moves to a `SceneQueryPool` lease composed with the viewer's load set. That lease is outside the viewer's lock and is decided at T3's verify.
- Time Samples on an attribute with no samples: the tab is disabled. Thousands of samples are listed in slices as the Layer tab reads text. Array values in a sample row are cut, with the full value on double-click.
- Value clips: samples come from `UsdAttribute::GetTimeSamples`, which includes the clips, and their rows are coloured violet.
- Layer Stack on an instance proxy names the prototype's layers. A layer no node owns shows its file, and an anonymous layer no node recorded shows "anonymous".
- Theme: colours are applied as foreground roles, so the stale (greyed) state still overrides them.

**Assumptions:**
- `GetResolveInfo().GetSource()` reports `UsdResolveInfoSourceValueClips` for clip-driven values on the USD version built (USD 25.x).
- Reading the stage's layer stack (`GetLayerStack(true)`) and each arc's layer stack from the prim index is cheap enough for one lock call per arc.

## Phase 571.1: Value sources

- [ ] M571.P1.T1 — Computed values in SceneOps
  - files: `Engine/Nodes/Scene/SceneOps.{h,cpp}`, `Tests/Scene/SceneProvenance_Test.cpp`
  - approach: add `eSceneValueSourceComputed` and a blocked flag on `ScenePrimAttribute`. Add `readComputed(const SceneStage&, primPath, frame, framesPerSecond, which, ScenePrimAttribute*)`, one call per row: world and local transform (`UsdGeomXformCache`), world and local bound (`UsdGeomBBoxCache`, default and render purposes, extents hint on, loaded prims only), computed visibility and purpose (`UsdGeomImageable`), and bound material for all purposes and preview (`ComputeBoundMaterial`). It returns false for a row the prim cannot have.
  - verify: gtest: a translated sphere's world transform and bound; a non-xformable Scope gets no transform rows; an invisible parent makes the child's computed visibility invisible; a material bound on the parent resolves on the child; a blocked attribute reads blocked; an attribute driven by value clips (two clip files written to the test's temp dir) reports Value Clips at a clip frame and its default outside the clip range.
  - size: M

- [ ] M571.P1.T2 — Colour values by source
  - files: `Gui/ScenePrimInspector.cpp`, `Tests/gui/scene_prim_inspector.py`
  - approach: one colour table keyed by `SceneValueSource` plus Relationship and Not set. `InspectorTableModel` returns the colour as the Value cell's foreground (the stale colour still wins). Relationships tab targets are red. A legend line of coloured source names sits under the Attributes table (`sceneGraphInspectorLegend`). The Source column keeps its text.
  - verify: Xvfb script: on USDSphere → USDSetAttribute(radius) with an animated displayColor, `radius` reads blue, `displayColor` green, an unauthored `purpose` orange, and a relationship red (checked through the model's foreground role); the stale state greys them; a screenshot of the tab with the legend at 1920x1080 goes to the user.
  - size: M

- [ ] M571.P1.T3 — Computed rows and Decompose Transforms
  - files: `Gui/ScenePrimInspector.cpp`, `Tests/gui/scene_prim_inspector.py`
  - approach: read the computed rows in their own lock calls after the authored rows, in a "Computed" block at the top of Attributes coloured indigo. Add a Decompose Transforms toggle above the table (`sceneGraphInspectorDecompose`) that shows each transform row as Translate, Rotate and Scale rows (`GfMatrix4d::Factor`). Apply the World Bound fallback from Edge cases only if this task's measurement needs it.
  - verify: Xvfb script: a USDCamera under a USDXformPrim lists World Transform with the parent's translate folded in, and Decompose shows it as three rows; selecting a heavy prim on the Island flat file keeps the main-thread heartbeat within the M333 budget (100 ms CPU, 400 ms with blocking), with the measurement recorded in `## Decisions`; changing the selection mid-bound cancels the read.
  - size: M

## Phase 571.2: Tabs

- [ ] M571.P2.T1 — Time Samples tab
  - files: `Gui/ScenePrimInspector.cpp`, `Engine/Nodes/Scene/SceneOps.{h,cpp}` (sample listing by path), `Tests/gui/scene_prim_inspector.py`
  - approach: a tab (`sceneGraphTimeSamples`) for the attribute last selected in Attributes, with Frame, Time Code and Value columns. Samples come from `GetTimeSamples`, read in slices of rows per lock call, coloured green or violet by source, with the current frame's row highlighted. A row menu has Copy CSV for the selected rows. The tab is disabled when the attribute has no samples. `readsAtFrame` stays true only for the highlight.
  - verify: Xvfb script: an attribute keyed at frames 1, 10 and 20 lists three rows with their values, highlights the one at the current frame, and Copy CSV puts `frame,timeCode,value` lines on the clipboard; a static attribute disables the tab.
  - size: M

- [ ] M571.P2.T2 — Metadata for the selected property
  - files: `Gui/ScenePrimInspector.cpp`, `Engine/Nodes/Scene/SceneOps.{h,cpp}` (`readPropertyMetadata`), `Tests/gui/scene_prim_inspector.py`
  - approach: when an attribute or relationship is selected, the Metadata tab lists that property's metadata (`UsdProperty::GetAllMetadata`) with Authored By, and a header line names the property. Clearing the selection returns to the prim's metadata.
  - verify: Xvfb script: selecting `radius` on a USDSphere lists its `typeName` and `variability` fields and the header says `radius`; clearing the selection lists the prim's `specifier`.
  - size: M

- [ ] M571.P2.T3 — Layer Stack tab
  - files: `Engine/Nodes/Scene/SceneOps.{h,cpp}` (`getLayerStackRows`), `Gui/ScenePrimInspector.cpp`, `Tests/Scene/SceneProvenance_Test.cpp`, `Tests/gui/scene_prim_inspector.py`
  - approach: rows, strongest first, as a tree: the stage's root layer stack (session, root and nested sublayers), then one branch per arc of the selected prim with that arc's layer stack. Each row shows its layer (file or "anonymous"), its owning node through the existing owner registry, its offset and scale, and whether it holds a spec for the selected prim (a marker; those rows are bold). It reads without a prim (root stack only) and needs no frame. Double-clicking a node row opens its panel, as Opinions does.
  - verify: gtest: USDRead(file) → USDEditTransform → USDReference lists the edit layer above the file layer, the reference's layers under its arc, and marks the layers holding the prim's specs; Xvfb script: double-click opens USDEditTransform's panel; a screenshot of the tab goes to the user.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `SceneProvenance_Test` passes; `Tests/gui/scene_prim_inspector.py` passes under Xvfb at 1920x1080; the selection stall stays within the M333 budget on the Island flat file; the user OKs the screenshots of the coloured Attributes tab, Time Samples and Layer Stack.

## Decisions

- 2026-11-24 — **Milestone now (user):** colour each attribute in the prim inspector by value source (authored default, time-sampled, value clip, fallback or schema default, computed), plus Layer Stack and Composition tabs, auditing Solaris's Scene Graph Details tabs (Value, Time Samples, Metadata, Editor Nodes, Layer Stack, Composition).
- 2026-11-24 — The Composition tab already exists from M333, so this milestone audits it and only adds the unloaded-payload colour. Editor Nodes is covered by Opinions, and Value by the full-value window and M337 (planner).
