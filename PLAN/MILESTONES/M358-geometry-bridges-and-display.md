# M358 - Geometry Bridges And Display

Second member of F15 - Native Geometry. The bridges between native geometry and the scene
stack, and geometry display in the Viewer's 3D view. `GeoToUSD` works like Solaris's SOP
Import. It contributes a layer served by the `natrongeo` Sdf file format plugin, backed by the
node's `GeoDetail`, which partitions the detail into USD prims by the `path` attribute lazily.
The Viewer and `USDRender` read that same layer. Copying into an anonymous layer is an
explicit, slower opt-in. `USDToGeo` → Geo operators → `GeoToUSD` works like SOP Modify:
prims arrive packed, unpack on request, and write back as overrides on their source paths.
Shared design: M54 - Native Geometry Data Model's intro and Scope, `PLAN/DECISIONS/2026-11-08-native-geometry-follows-sops.md`, `PLAN/DECISIONS/2026-10-27-geometry-bridges-follow-solaris.md`.

Member of F15 - Native Geometry: branches off and merges into `feature/f15-native-geometry` (PLAN-FORMAT §9b); the Feature's UAT covers it.

## Scope

**In:**
- `GeoUsdConvert` (partitioning, the attribute name and class table, groups, packed primitives).
- The `natrongeo` plugin with per-partition lazy split caching and in-place reload.
- `GeoToUSD` (new prims, overrides, "Packed Primitives" knob: Point Instancer default, Native Instances, Unpack; "Copy Into Layer" opt-in) and `USDToGeo` (packed by default, unpack on request, writes `path`).
- Geometry display in the Viewer's 3D view on F4 - Heavy Scene Viewer's viewer stage, with the core SOP display toggles: point markers, point and primitive numbers, normals, group highlighting.

**Out:**
- Component picking and `GeoEdit` (M361 - Geometry Point Editing).
- Template/footprint display and attribute visualisers (M363 - Geometry Display Extras).
- Component-level selection or editing in the scene context (M232 - Scene Selection To Nodes owns scene picking).
- Copying geometry into a layer by default, and writing `natrongeo` layers to disk (none).

**Edge cases:** the bridge edge cases in M54 - Native Geometry Data Model's Scope apply here: missing override paths, changed topology, instance proxies, value-only reloads, no `path`, vertex and edge groups, empty details.

**Assumptions:**
- `SdfLayer::Reload(true)` on a file-format layer with an unchanged identifier diffs into field-level change notices. M358.P1.T2 checks this first and falls back to `SdfChangeBlock` edits.
- Unconfirmed: primitives of mixed types sharing one `path` become per-type child prims under that path, with a node warning.

Requires M54 - Native Geometry Data Model, M52 (`PrimSelector`), and F4 - Heavy Scene Viewer landed (through M336 - Heavy Scene Acceptance), since display builds on the viewer stage F4 replaces. All nodes are `NativeEffectBase` subclasses in `Engine/Nodes/Geometry/`, each registered through its own `.reg.inc` (the pattern in `Engine/Nodes/Scene/USDCard.reg.inc`), under `PLUGIN_GROUP_3D`. Native geometry uses SOP attribute names (`P`, `N`, `uv`, `Cd`, `pscale`, `orient`, `path`, `name`); only `GeoUsdConvert` maps them to USD names.

## Phase 358.1: Bridges

- [x] M358.P1.T1 — `GeoUsdConvert`: partitioning and the attribute table
  - files: `Engine/Nodes/Geometry/GeoUsdConvert.h`/`.cpp`, `Tests/GeoUsdConvert_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: the one pxr-linked translation layer. It partitions the detail into USD prims by the `path` primitive attribute, falling back to `name`, then to one prim per primitive type under the scope (mixed types under one path: Scope assumption). Each partition gathers its points, re-indexes its vertices and maps by type: polygons → `UsdGeomMesh`, polylines → `UsdGeomBasisCurves`, unconnected points → `UsdGeomPoints`. Attribute classes map detail → `constant`, primitive → `uniform`, point → `vertex`, vertex → `faceVarying`. Names map per Solaris (`P`→`points`, `N`→`normals`, `uv`→`primvars:st`, `Cd`→`primvars:displayColor`, `pscale`→`widths`, the rest → `primvars:<name>`). Primitive groups → `UsdGeomSubset`, point groups → `bool` primvar; vertex and edge groups are dropped and reported. Packed USD → `over` at its source path with `xformOp:transform`. Packed Geometry → Point Instancer, native instances or unpacked, by mode. Two faces over one table. A **field-level query** (partition + field → spec type, field names, `VtValue`) is what M358.P1.T2's `SdfAbstractData` answers from. **Authoring** onto an `SdfLayer` at a `UsdTimeCode` serves the copy opt-in. Reading back works on a `UsdPrim` and writes `path`.
  - verify: round-trip unit test on an anonymous layer — a detail with two `path` partitions, a point `Cd` attribute and a primitive group authors two `UsdGeomMesh` prims with matching points and indices, `primvars:displayColor` with `vertex` interpolation, and a `UsdGeomSubset`; converting back yields a detail with the same content hash; the field-level query returns the same values the authoring path wrote, field by field; a no-`path` detail falls back as specified.
  - size: L

- [x] M358.P1.T2 — `natrongeo` Sdf file format plugin backed by `GeoDetail`
  - files: `Engine/Nodes/Geometry/GeoSdfFileFormat.h`/`.cpp`, `Engine/Nodes/Geometry/GeoSdfData.h`/`.cpp`, `Engine/Nodes/Geometry/plugInfo.json` (+ install rule), `Engine/Nodes/Scene/RenderDelegates.cpp` (register the plug-in directory beside the delegates), `Tests/GeoSdfFileFormat_Test.cpp`
  - approach: the Solaris SOP-import model. An `SdfFileFormat` (format id and extension `natrongeo`, read-only: `CanRead` true, write and export refused) whose `Read` installs a `GeoSdfData : SdfAbstractData`. It answers `HasSpec`, `Get`, `List` and `VisitSpecs` lazily through M358.P1.T1's field-level query. The partition table (paths and types) is cheap and built on open. A partition's split arrays are built on the first field query that needs them and cached per (detail hash, path), so unvisited prims cost nothing. Register with the existing `registerPluginDirectory` path in `RenderDelegates.cpp`, which already calls `PlugRegistry::RegisterPlugins` at startup. Layer identity is **node only**, never the content hash: `<node-uuid>.natrongeo` plus file-format args for output, view and mode (new prims vs overrides). A process-wide registry maps the identifier to a provider that returns the node's current detail. When the detail hash changes, the owner calls `layer->Reload(true)`, and only partitions whose per-partition hash changed are rebuilt. Check the Scope assumption about field-level reload notices first. If it fails, push the diff through `SdfChangeBlock` edits and record that in `## Decisions`. No node logic here; the test drives a registry entry directly.
  - verify: unit test — `SdfLayer::FindOrOpen` on a registered `GeoConstant` identifier gives a stage whose mesh matches the detail; querying one of two partitions builds only that partition's arrays; changing one point value and reloading keeps the identifier, and `UsdNotice::ObjectsChanged` reports `points` under changed-info-only, not as a resync; a topology change resyncs that prim alone; the layer refuses `Save`.
  - size: L

- [ ] M358.P1.T3 — `GeoToUSD`
  - files: `Engine/Nodes/Geometry/GeoToUSD.h`/`.cpp`/`.reg.inc`, `Tests/GeoToScene_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: inputs Geometry (+ optional Scene to append to). Knobs: root scope path; "author time samples across range" (default off, since vertex data is never auto-baked); "Packed Primitives" (Point Instancer default, Native Instances, Unpack, as SOP Import); "Copy Into Layer" (default off). Output kind **scene**. By default the node contributes its `natrongeo` layer (M358.P1.T2) to the `ScenePayload` layer stack, over the optional Scene input, with the detail's partitions under the scope path. Nothing is copied, and the plugin converts on demand. The layer is keyed per M19's per-node read keys, and a detail-hash change reloads it in place. "Copy Into Layer" authors the detail into the node's anonymous layer through `GeoUsdConvert` instead. This is the slower path, and the knob's tooltip says so. An empty detail contributes an empty layer.
  - verify: unit test — a `GeoConstant` detail appears under the scope path on the composed stage, and the stack holds a `natrongeo` layer and no anonymous copy; re-evaluating with an unchanged detail re-emits the same layer handle; a changed point value keeps the layer identifier; 100 packed-geometry copies author one `UsdGeomPointInstancer` with 100 instances by default, and native instances or meshes in the other modes; with "Copy Into Layer" on, the same prims come from an anonymous layer instead.
  - size: M

- [x] M358.P1.T4 — `USDToGeo`: packed by default, unpack on request
  - files: `Engine/Nodes/Geometry/USDToGeo.h`/`.cpp`/`.reg.inc`, `Tests/SceneToGeo_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: the SOP Modify model. Inputs Scene. Knobs: `PrimSelector`, "Unpack" (default off), "flatten transforms" (bake world xform into `P`; enabled only when unpacked); time follows the frame. Packed: each matched imageable prim becomes one packed USD primitive (M54.P1.T7) with its source path, local and parent-world transforms and bounds, plus a `path` primitive attribute. No point arrays are read, so transform-only chains stay cheap. Unpacked: mesh/points/curves read through `GeoUsdConvert` into the one detail, each primitive's `path` set to its source prim path. Unsupported types, instance proxies and prims under instanceable prims are skipped and listed in the node's info message.
  - verify: unit test — `USDRead`(reference `.usda`) → `USDToGeo` gives one packed primitive per matched prim with the right `path` and transforms, and no topology; with Unpack on it yields the expected polygons in one point pool; with flatten also on, a translated prim's points move; `GeoToUSD` → `USDToGeo`(unpacked) is the identity on a detail.
  - size: M

- [ ] M358.P1.T5 — `GeoToUSD` writes back overrides on source paths
  - files: `Engine/Nodes/Geometry/GeoToUSD.cpp`, `Engine/Nodes/Geometry/GeoSdfData.cpp`, `Engine/Nodes/Geometry/GeoUsdConvert.cpp`, `Tests/GeoToSceneOverrides_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: the SOP Modify writeback. When a Scene input is connected, partitions whose `path` exists on the input stage are served as `over` specs on that path in the `natrongeo` layer (the overrides mode arg) instead of new prims under the scope path. Other partitions still go under the scope. Packed USD → `xformOp:transform` local transform (new world × inverse parent world). Unpacked → `points` and changed primvars, plus full topology when the counts differ from the source. Paths missing upstream become a node warning (Scope edge cases).
  - verify: unit test — `USDRead` → `USDToGeo`(packed) → a test-only node that translates each packed primitive through the `GeoDetail` API → `GeoToUSD` moves the source prims on the composed stage at their original paths and adds no new prims; the unpacked chain overrides `points` only; a pruned source path raises the warning.
  - size: M

## Phase 358.2: Display

- [ ] M358.P2.T1 — The Viewer's 3D view displays geometry nodes through the `natrongeo` layer
  - files: `Gui/SceneViewport.cpp`/`.h`, `Engine/ViewerInstance.h`/`.cpp`, `Gui/ViewerTab.cpp`
  - approach: built on the viewer stage as F4 - Heavy Scene Viewer leaves it (deferred, worker-composed; re-read `PLAN/DESIGN/2026-11-04-scene-graph-design.md` at claim). Extend the Viewer's input acceptance and kind-driven view switch to the geometry kind. When the Viewer's active input is geometry, the viewer stage opens that node's `natrongeo` layer (M358.P1.T2), the same layer `GeoToUSD` renders from, so viewing and rendering share one conversion. A detail-hash change reloads the layer in place, so scrubbing and value edits dirty points instead of rebuilding the stage (the 2026-10-25 decision). Packed USD primitives draw as their bounds, and packed geometry as instances. Picking returns the prim path, which maps back to the partition and its primitive numbers.
  - verify: manual — a `GeoPrimitive` node viewed in the Viewer's 3D view shows the mesh; scrubbing a time-varying upstream node updates it without a stage rebuild (no resync notice); clicking a prim reports its primitive numbers; a packed `USDToGeo` output shows bounds boxes.
  - size: M

- [ ] M358.P2.T2 — Core SOP display toggles in the geometry context
  - files: `Gui/SceneViewport.cpp`/`.h`, `Gui/ViewerTab.cpp` (toolbar toggles), `Gui/GeoDisplayGuides.h`/`.cpp`
  - approach: viewer toggles shown only when the active input is geometry: point markers, point numbers, primitive numbers, normals (`N`, as lines scaled by a viewer setting) and highlighting of a chosen group. All are drawn as guide prims (purpose `guide`) authored into a viewer-private session layer, the same layer M361.P1.T1 uses for markers. Nothing is drawn by a second GL pass outside Hydra. Number labels are drawn only for elements in view, capped per frame, so a 10M-point detail stays responsive.
  - verify: manual (Xvfb screenshots shared before sign-off) — each toggle shows on a `GeoPrimitive` cube and clears when switched off; viewing a `GeoToUSD` downstream offers none of them; a 1M-point grid with point numbers on keeps the viewer interactive.
  - size: M

**Verification gate:** CI green with every Phase 358.1 test; on the Feature branch build under Xvfb, a `GeoPrimitive` chain displays in the Viewer's 3D view through its `natrongeo` layer, each display toggle works (screenshots shared before sign-off), scrubbing dirties points without a stage rebuild, and `GeoToUSD` → `USDRender` renders an image with no copied layer; the pre-existing ctest suite is green.

## Decisions

- 2026-11-08 — Speculative Phase 1 (PM, user: proceed ahead of deps): T1, T2, T4 written on the M54 branch. The natrongeo layer refreshes with its own edits inside one SdfChangeBlock (only changed specs) rather than SdfLayer::Reload, whose notices for a dynamic file format are unconfirmed; it registers next to the live-texture plugins in AppManager (nothing calls RenderDelegates::registerPluginDirectory). USDToGeo evaluates its scene input itself and is always frame-varying. T3 and T5 are blocked: every scene evaluation holds a SceneWriteBarrier::ReadScope (EffectInstanceRenderScene.cpp ~105) so GeoToUSD can't refresh its layer during renderScene; needs M336 - Heavy Scene Acceptance's F4 work.

- 2026-11-08 — Split out of M54 - SOPs-Style Geometry (user, /cat-plan); tasks renumbered from M54.P2.*. Waits on F4 - Heavy Scene Viewer landing (user, M54 grill).
