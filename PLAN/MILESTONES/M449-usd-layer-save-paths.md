# M449 - USD Layer Save Paths

One USDWrite can write a whole layer set: a root file that sublayers named layer files, nested as deep as the graph says. A `USDConfigureLayer` node names a **region** of the stack, with a Save Path and layer metadata. Its `Applies To` choice either gathers the layers before it (`Preceding Layers`) or opens a region for the layers after it (`Following Layers`). Optional `USDLayerBegin` and `USDLayerEnd` nodes bracket a region explicitly. Regions nest: the outer file sublayers the inner one, so `shot.usd → lighting.usd → lights_key.usd` comes from one graph. Builds on M448 - USD Layer Break's per-entry flag. Second member of F27 - USD Layer Authoring.

Regions live on the stack, like M448's context flag. `SceneLayerEntry` gains a pointer to the region that holds it, and `ScenePayloadImpl` carries the stack of regions still open. USDWrite turns each region into a file and keeps its position in its parent's layer order, using the run/sidecar machinery `layOutStack` already has.

## Scope

**In:**
- `USDConfigureLayer`, which has knobs `appliesTo` ("Applies To": Preceding Layers | Following Layers, default Preceding Layers) and `savePath` ("Save Path"). Behind `AuthoredKnobSet` toggles it also has `defaultPrim`, `comment`, `startTime`, `endTime`, `timeCodesPerSecond`, `framesPerSecond`, `upAxis`, `metersPerUnit`, `colorConfiguration`, `colorManagementSystem` and `renderSettingsPrimPath`, plus custom-data rows of name, type and value. The types are String, String Array, Token, Token Array, Float, Integer and Boolean.
- `USDLayerBegin` and `USDLayerEnd`, bare markers with no knobs.
- USDWrite writes each region to its file, nested by sublayers. It resolves relative save paths against its own output directory, with M245 - Write Path Variables applying. It gains `saveCollisions` ("Save Path Collisions": Error | Merge Into One File, default Error).
- A third USDWrite Export Mode, `Flatten Layers` (between Layered and Flattened). It writes the whole layer stack into the one output file, read files included, with no sublayers and no side files. References, payloads and variants stay live.
- Node docs, the USDWrite doc update, ctest coverage and one Xvfb GUI script.

**Out:**
- The preview tree and the pre-render node error: M450 - USDWrite Export Preview.
- Per-frame files, value clips and extra save styles: the user scoped them out (2026-11-15).
- A save-path knob on every scene node: the user chose the dedicated node.
- Output processors or Python hooks on the save: no milestone.

**Edge cases:**
- Preceding Layers gathers every non-context entry since the last boundary on each path. A boundary is a read file (an external entry), a break, another region's boundary, or a merge. Across a USDMerge it reaches into every input, so a Preceding-mode node after a merge puts several branches' unclaimed layers in one file. It stops at a read file because flattening layers on both sides of one would change their strength against it.
- Following Layers and Begin open a region. It holds every entry appended until the matching End, a merge, a break, or the USDWrite, and it holds nested regions as sublayers.
- A region still open at a USDMerge closes at that input. Regions never cross a merge, so another branch's layers never land in its file.
- A second Begin or Following-mode node inside an open region nests. End closes the innermost region. An End with no open region does nothing and shows a warning.
- Begin alone names no file. A USDConfigureLayer inside the region, in either mode, sets its save path and metadata. Without one, the region is implicit and is flattened into its parent's file.
- A region above a break is context and nothing of it is written.
- A read file inside a region is sublayered by that region's file, not the root.
- Two regions resolving to the same file: by default USDWrite fails before writing anything and names both USDConfigureLayer nodes. With Merge Into One File, both regions flatten into that file in stack order. A region resolving to USDWrite's own output, or to a file the scene reads, is always an error, whatever that setting.
- A relative Save Path resolves against USDWrite's output directory. An absolute one stays absolute. References between written files follow USDWrite's existing `pathMode`.
- Region files are named by the user, so the stale-sidecar sweep (`removeStaleSidecars`) never deletes them. Unnamed runs keep today's `_layerN` sidecar names.

- Flatten Layers ignores regions and save paths: everything goes into the output file, and region metadata follows the rule for blank-path regions. Breaks (M448 - USD Layer Break) are still honoured.
- Flatten Layers with a USDReference to a file on disk: the reference stays an external reference. Only USDRead's sublayered files are flattened in.
- Flatten Layers with a USDReference to another branch of the graph (an anonymous stack): that stack is flattened into the output under a `class` root prim, `/_references/stack<N>`, and the arc becomes an internal reference to it. No side file is written.

**Assumptions:**
- Unconfirmed: inlined referenced stacks live under `/_references/stack<N>` as `class` prims. A `class` composes no visible prims of its own, so the stage looks the same; check the name at UAT.
- Unconfirmed: a USDConfigureLayer with a blank Save Path and metadata set applies that metadata to the file that receives its region (the parent's file, or the root).
- Unconfirmed: Up Axis, Meters Per Unit, the time range and the other stage-level fields set on a region that lands in the root file override USDWrite's root metadata (`authorStageMetadata`). On any other region they apply only to that region's file.
- Region entries are contiguous in the flattened stack by construction: Following regions are appended in one run, merges close them, and Preceding claims a run. USDWrite asserts this and fails with a clear message if it is ever false.

## Phase 449.1: Regions on the stack

- [x] M449.P1.T1 — Region model in the scene payload (PR #359)
  - files: `Engine/Nodes/Scene/ScenePayloadImpl.h`, `Engine/ScenePayload.h` (opaque accessors only, no USD types), `Engine/Nodes/Scene/SceneOps.h`/`.cpp`, `Engine/Nodes/Scene/USDMerge.cpp`, `Tests/Scene/SceneLayerRegion_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: add an immutable `SceneLayerRegion` with an id, the authoring node's name, a parent pointer and `SceneLayerRegionSettings` (save path, plus each optional metadata field and custom data). Add `std::shared_ptr<const SceneLayerRegion> region` to `SceneLayerEntry` and an open-region stack to `ScenePayloadImpl`. Add these `SceneOps` functions:
    - `openRegion(payload, settings)`
    - `closeRegion(payload)`
    - `setOpenRegionSettings(payload, settings)`, which merges settings into the innermost open region
    - `claimPreceding(payload, settings)`, which retags the run back to the last boundary on each path into a new region whose parent is the current open region
    - `regionsOf(payload)`

    `appendEntry` tags new entries with the innermost open region. Fold region ids and settings into the stack hash. `mergeStacks` closes every input's open regions. `markContext` (M448) closes them as well.
  - verify: unit tests.
    - Open, append twice, close: both entries share the region.
    - A nested open gives the inner region the outer one as parent.
    - Preceding after Read → A → B claims A and B, not the read entry.
    - Preceding after a merge of two branches claims both branches' unclaimed entries.
    - An open region at a merge input is closed in the merged payload.
    - Every change alters the hash.
  - size: L

- [x] M449.P1.T2 — `USDLayerBegin` and `USDLayerEnd` nodes (PR #359)
  - files: `Engine/Nodes/Scene/USDLayerBegin.h`/`.cpp`/`.reg.inc`, `Engine/Nodes/Scene/USDLayerEnd.h`/`.cpp`/`.reg.inc`, their `docs/*.md` pages and icons, `Tests/Scene/SceneLayerRegion_Test.cpp`
  - approach: knobless scene nodes on the USDLayerBreak pattern (M448.P1.T2). Begin returns `openRegion(input, {})` and End returns `closeRegion(input)`. End with no open region passes its input through and sets a warning persistent message. The docs explain regions, nesting, and that a merge closes them.
  - verify: unit test. Begin → edit → End → edit: the first edit is in a region and the second is not. End alone warns and passes through.
  - size: M

- [x] M449.P1.T3 — `USDConfigureLayer` node (PR #359)
  - files: `Engine/Nodes/Scene/USDConfigureLayer.h`/`.cpp`/`.reg.inc`, `Engine/Nodes/Scene/docs/USDConfigureLayer.md`, icon, `Tests/Scene/SceneLayerRegion_Test.cpp`
  - approach: one Scene input. Its knobs are `appliesTo` and `savePath` (a `KnobString` holding a file path, with path variables allowed). The metadata knobs from `## Scope` sit in an `AuthoredKnobSet` (M52.P1.T3), so only toggled fields are recorded. `upAxis` is a choice of Y | Z, and `colorConfiguration` is a file knob. Labels match script names. Preceding Layers returns `claimPreceding(input, settings)`. Following Layers returns `openRegion(input, settings)`. Inside an already-open Begin region with nothing before it in that region, it sets that region's settings instead (`setOpenRegionSettings`). The node authors no USD layer.
  - verify: unit tests. Each mode tags the expected entries. Only toggled fields appear in the region settings. A Following-mode node right after a Begin sets the Begin region's settings rather than nesting.
  - size: M

- [x] M449.P1.T4 — Custom data rows on `USDConfigureLayer` (PR #359)
  - files: `Engine/Nodes/Scene/USDConfigureLayer.cpp`, `Engine/Nodes/Scene/docs/USDConfigureLayer.md`, `Tests/Scene/SceneLayerRegion_Test.cpp`
  - approach: add a `customData` table knob with columns Name, Type and Value, following an existing `KnobTable` user (see `Gui/KnobGuiTable.cpp` for the GUI side). Values are parsed per type: arrays split on whitespace, String Array respects quotes, Token Array does not. A row that fails to parse is a node error naming the row. The parsed entries go into the region settings' custom data.
  - verify: unit test. One row of each type parses to the expected `VtValue` kind, and an Integer row holding text is an error.
  - size: M

## Phase 449.2: USDWrite writes the region tree

- [x] M449.P2.T1 — One file per region, nested by sublayers
  - files: `Engine/Nodes/Scene/USDWrite.cpp` (`StackItem`, `appendLayerStack`, `layOutStack`, `writeFrameRange`, `removeStaleSidecars`), `Engine/Nodes/Scene/docs/USDWrite.md`
  - approach: carry each entry's region on its `StackItem`. In layered mode, build the region tree from the root stack's items. Each named region becomes a written stack whose path is its save path, resolved against the output directory and expanded with the frame-1 path variables as USDWrite's own file is. Its contiguous items are replaced in the parent by one sublayer item pointing at it. Implicit regions (no save path) stay inline in their parent. Lay out and write each stack with the existing run/sidecar logic, recursively. Root stage metadata stays as today. Apply the overwrite guard ("is read by the scene") to every region file. The stale-sidecar sweep only touches `_layerN` names. Non-contiguous region items are a write error naming the region's node. Flattened mode ignores regions.
  - verify: `Tests/Scene/USDWrite_Test.cpp` gains `SaveRegionsWriteNestedLayerFiles`. Read → A → B → ConfigureLayer(Preceding, `lighting.usda`) → Begin → ConfigureLayer(Following, `key/lights_key.usda`) → C → End → D → USDWrite(`shot.usda`). Expected files:
    - `shot.usda` holds D and sublayers `lighting.usda` and `key/lights_key.usda` in stack order.
    - `lighting.usda` holds A and B and sublayers the read file.
    - `lights_key.usda` holds C.
    - Recomposing `shot.usda` gives the graph's composed stage.
  - size: L

- [x] M449.P2.T2 — Region metadata on written files
  - files: `Engine/Nodes/Scene/USDWrite.cpp` (`authorStageMetadata` and the per-stack write in `writeFrameRange`), `Tests/Scene/USDWrite_Test.cpp`
  - approach: author each named region's recorded settings on its file's root layer. Default prim, comment, start/end time, tcps, fps, upAxis, metersPerUnit, colour configuration and system, and the render settings prim go to their `SdfLayer` / `UsdStage` metadata fields. Custom data goes to the layer's `customLayerData`. An implicit region's settings go to the file that receives it. A region landing in the root overrides the root fields that `authorStageMetadata` would set.
  - verify: test `SaveRegionMetadataIsAuthored`. A region with upAxis Z, a comment and one custom-data row writes those to its file's layer metadata. An untoggled field is absent. A blank-path region's comment lands on the root file.
  - size: M

- [x] M449.P2.T3 — `saveCollisions` knob
  - files: `Engine/Nodes/Scene/USDWrite.h`/`.cpp`, `Engine/Nodes/Scene/docs/USDWrite.md`, `Tests/Scene/USDWrite_Test.cpp`
  - approach: add `saveCollisions` (`KnobChoice`, label "Save Path Collisions", entries Error | Merge Into One File, default Error). Before writing anything, map resolved paths to regions. Under Error, fail if two regions share a path, naming both nodes. Under Merge Into One File, concatenate the regions' items in stack order into one written stack, and merge their settings with the stronger region winning per field. A region resolving to the output file is an error in both modes.
  - verify: tests. Two branches saving to `lights.usda` fail under Error with both node names in the message and no file written. Under Merge they produce one file holding both branches' edits. A region named like the output always fails.
  - size: M

- [x] M449.P2.T4 — Region interplay tests
  - files: `Tests/Scene/USDWrite_Test.cpp`
  - approach: four tests.
    - A region above a USDLayerBreak writes no file.
    - A Following region open at a USDMerge closes there, so the other branch's edits land in the root.
    - Preceding after a merge gathers both branches into one file.
    - An End with no open region writes as if absent.
  - verify: the four tests pass, along with every existing `USDWrite_Test` and M448 test.
  - size: M

- [x] M449.P2.T5 — GUI script for layer regions
  - files: `Tests/gui/m449_layer_save_paths.py`, GUI test registration
  - approach: under Xvfb, build P2.T1's graph from Python, set ConfigureLayer knobs through the Python API (including one custom-data row), render USDWrite, and check the written file set and the root file's sublayer list by reading them back.
  - verify: the script passes in the remote `--gui` gate.
  - size: M

- [x] M449.P2.T6 — `Flatten Layers` export mode
  - files: `Engine/Nodes/Scene/USDWrite.h`/`.cpp` (`initializeKnobs` exportMode entries, `writeFrameRange`, `appendLayerStack`), `Engine/Nodes/Scene/docs/USDWrite.md`, `Tests/Scene/USDWrite_Test.cpp`
  - approach: add `flattenLayers` ("Flatten Layers") as the middle `exportMode` entry, with a tooltip contrasting it with Layered (a thin file sublayering read files) and Flattened (composition baked in). For the root stack, collect items as Layered does, after context skipping, but treat external items (read files) like anonymous ones. Flatten the whole run with `flattenRun` / `UsdUtilsFlattenLayerStack` into the output layer, so no sublayers remain. Regions are ignored. Asset paths inside flattened read files resolve through the existing `UsdUtilsFlattenLayerStackResolveAssetPath` callback, then `pathMode` applies. Referenced anonymous stacks still go to side files until P2.T7. Stage metadata as Layered.
  - verify: test `FlattenLayersWritesOneFileKeepingArcs`. Read the M52 layer-stack fixture → USDVariant → edit → USDWrite in Flatten Layers mode. The output has no sublayers. It holds the fixture's prims and the edit, and the variant set with both variants and the authored selection. A USDReference to `m52-referenced.usda` on disk stays a reference to that file. Recomposing it gives the graph's stage.
  - size: M

- [x] M449.P2.T7 — Inline referenced stacks in `Flatten Layers`
  - files: `Engine/Nodes/Scene/USDWrite.cpp` (`anonymousArcs`, the `stackOfLayer` and arc-rewrite step of `writeFrameRange`), `Tests/Scene/USDWrite_Test.cpp`
  - approach: in Flatten Layers mode, flatten each anonymous referenced stack (`stacks[i]`, i > 0) as P2.T6 does. Then copy its root prims under `/_references/stack<N>` in the output layer, where N follows today's side-file numbering, with that root prim specified as `class` (`SdfCopySpec`). Rewrite each arc to an internal reference (empty asset path) to the copied prim: the arc's prim path, or the stack's default prim, re-rooted under `stack<N>`, keeping the layer offset (`scaleArcOffsets` applies as today). Relationship and connection targets inside copied prims are re-rooted with the same path mapping. Nested referenced stacks go in recursively. A stack referenced twice is copied once.
  - verify: test `FlattenLayersInlinesReferencedStacks`. A USDReference whose source is a second Read → edit branch, plus a USDXformPrim graft, writes one file and no side files. Recomposing it gives the graph's composed stage, prim for prim and attribute for attribute at two frames. A material binding inside the referenced stack still resolves.
  - size: L

**Verification gate:** CI green, including the region, ConfigureLayer and USDWrite tests and `m449_layer_save_paths.py` under `--gui`. A single USDWrite produces the nested file set of M449.P2.T1, and recomposing it gives the graph's stage. Collisions error by default and merge on request. Flatten Layers writes one self-contained file whose composed stage matches the graph's, with references and variants live. User-facing checks (building a real shot/lighting/key layer set) are collected at F27 - USD Layer Authoring's UAT.

## Decisions

- 2026-11-15 — Save paths live on a dedicated USDConfigureLayer node, not a knob on every scene node (user, grill).
- 2026-11-15 — Both grouping behaviours, chosen by `Applies To`: Preceding gathers the run since the last boundary, and Following opens a region (user, grill: "1 and 2, configurable"). Optional bare USDLayerBegin/USDLayerEnd markers bracket regions; Begin carries no settings, a ConfigureLayer inside the region does (user, grill).
- 2026-11-15 — Regions nest as sublayers; End closes the innermost (user, grill).
- 2026-11-15 — Open regions close at a USDMerge; a Preceding-mode ConfigureLayer after the merge gathers several branches into one file (user, grill, after asking how a multi-branch file would be made).
- 2026-11-15 — Full layer metadata set including custom data rows, each behind an authored toggle (user, grill).
- 2026-11-15 — Relative save paths resolve against USDWrite's output directory (user, grill): one graph writes a whole layer set to any shot directory.
- 2026-11-15 — Save-path collisions are configurable on USDWrite: Error by default, Merge Into One File on request (user, grill).
- 2026-11-15 — USDWrite gains a third Export Mode, Flatten Layers: one self-contained file with every layer (read files included) flattened in but composition arcs kept (user, after the grill: "choose whether to write everything into the output file or a thin file referencing external files"). Layered stays the thin option. Referenced graph stacks are inlined as internal references so no side files remain (planner); file references stay external.
- 2026-11-23 — Phase 1 implemented (gate queued); Phase 2 (USDWrite) next. PM defaults: a Read file is a region boundary for Preceding; a ConfigureLayer inside a bare Begin region configures it; Float custom data is stored as double. Open for Phase 2: regions made non-contiguous by a Preceding claim after a merge need a USDWrite rule; nodes that build a fresh stack or return an empty scene close open regions.
- 2026-11-23 — Phase 2 in a77adcde5 (gate queued). PM choices: a region made non-contiguous by a Preceding claim takes the layers between into its own file (two interleaved regions fail, naming both); regions above a break write no file when Strip Layers Above Breaks is on; regions inside referenced or grafted stacks are not written as files; a region without a Save Path puts its metadata on the file holding its layers; region time codes per second are metadata only; a gathering region takes the file entry just below it (so lighting.usda sublayers the read file, per the spec); knob label Save Collisions (matches its script name). Python can't read the customData knob table yet (no KnobTable wrapper), so the GUI script skips that check.

- 2026-11-24 (PM, review #359): region rates stay metadata — sublayer offsets cancel USD rate scaling. Known limit: a Save-Path-less region setting a rate on the root file compensates sublayers but not references/payloads (USD scales those by the stage rate). A Save Path named <output>_layerN.<ext> beside the output is refused. PLUGIN_GROUP_3D_USD → M520 submenus resolved when F36 meets F27.
