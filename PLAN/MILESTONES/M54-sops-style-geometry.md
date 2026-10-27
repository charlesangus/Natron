# M54 - SOPs-Style Geometry

Full title: SOPs-style geometry — GeoDetail data kind, bridges, operators, point editing

Stage 3 of the 3D roadmap (`PLAN/DESIGN/2026-09-18-3d-roadmap-lops-paint-sops.md`,
Question 3). A fourth data kind, `eDataKindGeometry`, carrying a
copy-on-write `GeoDetail` (UsdGeom's attribute vocabulary without a
stage); explicit bridges to and from the scene stack; the Viewer's
3D view for display; a Nuke-classic-sized operator set plus a Python
wrangle; and EditGeo-style point editing.
Decision: `PLAN/DECISIONS/2026-09-18-geometry-is-a-fourth-data-kind.md`.

The bridges follow Solaris (`PLAN/DECISIONS/2026-10-27-geometry-bridges-follow-solaris.md`).
`GeoToUSD` works like SOP Import. It doesn't copy the detail into a layer. It contributes a
layer served by an Sdf file format plugin backed by the node's `GeoDetail`, and the plugin
converts lazily. The Viewer and `USDRender` read that same layer. Copying into an
anonymous layer is an explicit, slower opt-in. `USDToGeo` → Geo operators → `GeoToUSD`
works like SOP Modify. Prims arrive as packed prims that can be transformed without
unpacking, unpack happens only on request, and the result is written back as overrides on
the source prim paths. Component-level display, picking and editing exist only in the
geometry context. In the scene context, picking and editing stay at prim level, as in LOPs.

## Scope

**In:**
- `GeoDetail` with mesh, points, curves and packed prims; its cache and pull path.
- The `natrongeo` Sdf file format plugin, `GeoToUSD` (new prims and overrides), and `USDToGeo` (packed by default, unpack on request).
- Geometry display in the Viewer's 3D view through the plugin layer; component picking and highlighting in the geometry context only.
- The P3 operators, `GeoWrangle` and `GeoEdit`.

**Out:**
- Component-level selection or editing in the scene context. USD nodes select and edit whole prims (M232 - Scene Selection To Nodes owns scene picking).
- Copying geometry into a layer by default. Copying happens only through `GeoToUSD`'s opt-in knob.
- A standalone `GeoUnpack` node and full-shape display of packed prims. Packed prims carry no topology, so unpacking happens in `USDToGeo`, where the stage is available. Packed prims display as bounds.
- Writing `natrongeo` layers to disk in their own format. `USDWrite` exports the composed result as usual.

**Edge cases:**
- An override targets a path that no longer exists upstream (the prim was pruned or renamed after `USDToGeo`). The `over` is inert in USD, and `GeoToUSD` lists the missing paths in a node warning.
- An unpacked prim's topology changed between `USDToGeo` and `GeoToUSD`. The override authors the full topology (`faceVertexCounts`, `faceVertexIndices`, every primvar) as well as `points`.
- Instance proxies and prims under an instanceable prim can't take overrides. `USDToGeo` skips them and lists them in its info message.
- Value-only changes such as scrubbing a time-varying upstream or editing a point keep the layer identifier, so Hydra dirties points and primvars instead of resyncing the prim. A topology change resyncs only that prim.

**Assumptions:**
- `SdfLayer::Reload(true)` on a file-format layer with an unchanged identifier diffs the new data against the old and sends field-level change notices, not a full resync. M54.P2.T5 verifies this first. If it's false, the fallback is to push the diff through `SdfChangeBlock` edits.
- A custom `SdfFileFormat` can be registered at startup from a `plugInfo.json` shipped with Natron via `PlugRegistry::RegisterPlugins`. Nothing in the tree registers a USD plug-in yet.
Requires M52 (`PrimSelector`) and M20.P2.T1 (gizmo). Runs after M53.
All nodes are `NativeEffectBase` subclasses in `Engine/Nodes/Geometry/`,
registered under `PLUGIN_GROUP_3D`; mirror M18's deep plumbing wherever a
choice arises.

## Phase 54.1: Data kind, container, cache, pull path

- [ ] M54.P1.T1 — `eDataKindGeometry` declared and drawn
  - files: `Global/Enums.h`, `Gui/Edge.cpp`, `Gui/NodeGui.cpp`, `Tests/DataKind_Test.cpp`, `Engine/Nodes/README.md`
  - approach: add the fifth `DataKindEnum` value; edge styling and node silhouette per the 2026-09-05 design doc's "Communicating kind" rules (distinct from image/deep/scene); connection typing needs no change — it is kind-agnostic. Document the kind in the README's "Data kinds" section with the same "declaration only until 54.1 lands" caveat deep carried.
  - verify: `DataKind_Test` extended — a geometry source into a scene input is refused, geometry → `Dot` → geometry consumer resolves; Xvfb GUI screenshot shows the new edge style.
  - size: M

- [ ] M54.P1.T2 — `GeoDetail` value type
  - files: `Engine/GeoDetail.h`/`.cpp` (pimpl, no pxr header), `Engine/Nodes/Geometry/GeoDetailImpl.h`/`.cpp`, `Tests/GeoDetail_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: per the design doc's container sketch — `prims` (kind mesh/points/curves/packed, path token, optional source prim path, `VtArray` points/counts/indices, attribute map keyed by (interpolation, name) → `VtValue`, groups name → bitset), `contentHash` (U64 over all arrays, computed lazily and cached), `getSizeInBytes()` aliasing-blind like `DeepImage`. Mutation API returns a new detail sharing untouched arrays (`VtArray` COW). Public header exposes typed accessors (`const float*`/counts) so `Engine/` core and tests never see pxr. A **packed** prim is Houdini's packed primitive: one point with no topology, carrying its source prim path, its local transform, its parent's world transform (so a writeback can turn a new world transform back into a local one) and its bounds.
  - verify: unit tests — copying then mutating `points` detaches only `points` (other arrays share storage, checked via `VtArray::IsIdentical`); identical content hashes identically; changing a packed prim's transform changes the hash and detaches nothing else; `Engine/GeoDetail.h` includes no pxr header (grep-enforced, as M19.P1.T2).
  - size: M

- [ ] M54.P1.T3 — `Cache<GeoDetail>` with its own budget
  - files: `Engine/GeoDetailKey.h`/`.cpp`, `Engine/GeoDetailCacheEntry.h`/`.cpp`, `Engine/Settings.cpp`/`.h` (budget knob), `Tests/GeoDetailCache_Test.cpp`
  - approach: third instantiation of the templated cache, mirroring `DeepImageKey`/`DeepImageCacheEntry` from M18 exactly: key = (nodeHash, time, view); cost = `getSizeInBytes()`, a pure function of shape at allocate and deallocate (the 2026-09-09 amendment rule); separate memory budget knob so geometry never evicts the image cache.
  - verify: unit test — store/fetch by key; budget clamp evicts; counter returns to zero after eviction.
  - size: M

- [ ] M54.P1.T4 — `renderGeometry()` capability virtual and pull path
  - files: `Engine/EffectInstance.h`/`.cpp`, `Engine/Nodes/NativeEffectBase.h`/`.cpp`, `Tests/GeoRender_Test.cpp`
  - approach: mirror the deep path M18 added (`renderDeep()` and its `renderDeepRoI`-shaped pull): `EffectInstance::renderGeometry(const GeoRenderActionArgs&, GeoDetailPtr*)` plus a pull that walks typed inputs, honours `getFramesNeeded()`, abort flags and the scheduler, consults `Cache<GeoDetail>` first, and ignores RoI (whole-detail pull, as scene). `NativeEffectBase` gains `renderGeometryFromInput(args, input, mutate)` (fetch input detail, hand a COW copy to `mutate`) and a generator variant. Trivial proof node `GeoConstant` (one triangle) used by the test only (`internalUseOnly = true`).
  - verify: unit test — `GeoConstant` → passthrough → consumer pulls the detail through the cache (second pull is a cache hit); abort mid-pull returns cleanly.
  - size: L

## Phase 54.2: Bridges and viewing

- [ ] M54.P2.T1 — `GeoUsdConvert`: detail ↔ UsdGeom lookup table
  - files: `Engine/Nodes/Geometry/GeoUsdConvert.h`/`.cpp`, `Tests/GeoUsdConvert_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: both directions: detail prim ↔ `UsdGeomMesh`/`UsdGeomPoints`/`UsdGeomBasisCurves`, attributes ↔ primvars with interpolation, groups ↔ `UsdGeomSubset` (prim groups) or a `bool[]` primvar (point groups), packed prim ↔ `over` at its source path carrying an `xformOp:transform` (local transform) and `xformOpOrder`. Two faces over one table: a **field-level query** (prim index + field → spec type, field names, `VtValue`) that M54.P2.T5's `SdfAbstractData` answers from without materialising anything, and **authoring** onto an `SdfLayer` at a given `UsdTimeCode` for `GeoToUSD`'s copy opt-in. Reading back works on a `UsdPrim`. No node logic here.
  - verify: round-trip unit test on an anonymous layer — a detail with a mesh, a `vertex` color attribute and a prim group authors a `UsdGeomMesh` with matching points/indices, a `primvars:color` with `vertex` interpolation and a `UsdGeomSubset`; converting back yields an identical detail (same content hash); the field-level query returns the same values the authoring path wrote, field by field.
  - size: M

- [ ] M54.P2.T5 — `natrongeo` Sdf file format plugin backed by `GeoDetail`
  - files: `Engine/Nodes/Geometry/GeoSdfFileFormat.h`/`.cpp`, `Engine/Nodes/Geometry/GeoSdfData.h`/`.cpp`, `Engine/Nodes/Geometry/plugInfo.json` (+ install rule), `Engine/Nodes/Scene/` USD init (`PlugRegistry::RegisterPlugins` once at startup), `Tests/GeoSdfFileFormat_Test.cpp`
  - approach: the Solaris SOP-import model. An `SdfFileFormat` (format id and extension `natrongeo`, read-only: `CanRead` true, write and export refused) whose `Read` installs a `GeoSdfData : SdfAbstractData` that answers `HasSpec`, `Get`, `List` and `VisitSpecs` lazily from a `GeoDetail` through M54.P2.T1's field-level query. Layer identity is **node only**, never the content hash: `<node-uuid>.natrongeo` plus file-format args for output, view and mode (new prims vs overrides). A process-wide registry maps the identifier to a provider that returns the node's current detail. When the detail hash changes, the owner calls `layer->Reload(true)`. First, check the Scope assumption that Sdf diffs a reload into field-level notices. If it doesn't, push the diff through `SdfChangeBlock` edits and record that in `## Decisions`. No node logic here; the test drives a registry entry directly.
  - verify: unit test — `SdfLayer::FindOrOpen` on a registered `GeoConstant` identifier gives a stage whose mesh matches the detail; changing one point value and reloading keeps the identifier, and `UsdNotice::ObjectsChanged` reports the `points` attribute under changed-info-only, not as a resync; a topology change resyncs that prim alone; the layer refuses `Save`.
  - size: L

- [ ] M54.P2.T2 — `GeoToUSD`
  - files: `Engine/Nodes/Geometry/GeoToUSD.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoToScene_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: inputs Geometry (+ optional Scene to append to); knobs: root scope path, "author time samples across range" (default off; vertex data is never auto-baked), "Copy Into Layer" (default off). Output kind **scene**. By default the node contributes its `natrongeo` layer (M54.P2.T5) to the `ScenePayload` layer stack, over the optional Scene input, with the detail's prims under the scope path. Nothing is copied, and the plugin converts on demand. The layer is keyed per M19's per-node read keys, and a detail-hash change reloads it in place. "Copy Into Layer" authors the detail into the node's anonymous layer through `GeoUsdConvert` instead. This is the slower path; the knob's tooltip says so.
  - verify: unit test — a `GeoConstant` detail appears under the scope path on the composed stage, and the stack holds a `natrongeo` layer and no anonymous copy; re-evaluating with an unchanged detail re-emits the same layer handle; a changed point value keeps the layer identifier; with "Copy Into Layer" on, the same prims come from an anonymous layer instead.
  - size: M

- [ ] M54.P2.T3 — `USDToGeo`: packed by default, unpack on request
  - files: `Engine/Nodes/Geometry/USDToGeo.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/SceneToGeo_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: the SOP Modify model. Inputs Scene; knobs: `PrimSelector`, "Unpack" (default off), "flatten transforms" (bake world xform into points; enabled only when unpacked), time follows the frame. Packed: each matched imageable prim becomes one packed prim (M54.P2.T1's table) with its source path, local and parent-world transforms and bounds. No point arrays are read, so transform-only chains stay cheap. Unpacked: mesh/points/curves through `GeoUsdConvert` over the composed input stage, each recording its source path. Unsupported types, instance proxies and prims under instanceable prims are skipped and listed in the node's info message.
  - verify: unit test — `USDRead`(reference `.usda`) → `USDToGeo` gives one packed prim per matched prim with the right paths and transforms, and no points; with Unpack on it yields the expected mesh; with flatten also on, a translated prim's points move; `GeoToUSD` → `USDToGeo`(unpacked) is the identity on a detail.
  - size: M

- [ ] M54.P2.T6 — `GeoToUSD` writes back overrides on source paths
  - files: `Engine/Nodes/Geometry/GeoToUSD.cpp`, `Engine/Nodes/Geometry/GeoSdfData.cpp`, `Engine/Nodes/Geometry/GeoUsdConvert.cpp`, `Tests/GeoToSceneOverrides_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: the SOP Modify writeback. When a Scene input is connected, prims that carry a source path are served as `over` specs on that path in the `natrongeo` layer (the overrides mode arg) instead of new prims under the scope path. Prims without a source path still go under the scope. Packed prim → `xformOp:transform` local transform (new world × inverse parent world). Unpacked prim → `points` and changed primvars, plus full topology when the counts differ from the source. Paths missing upstream become a node warning (Scope edge cases).
  - verify: unit test — `USDRead` → `USDToGeo`(packed) → a test-only node that translates each packed prim's transform through the `GeoDetail` API → `GeoToUSD` moves the source prims on the composed stage at their original paths and adds no new prims; the unpacked chain overrides `points` only; a pruned source path raises the warning.
  - size: M

- [ ] M54.P2.T4 — The Viewer's 3D view displays geometry nodes through the `natrongeo` layer
  - files: `Gui/SceneViewport.cpp`/`.h`, `Engine/ViewerInstance.h`/`.cpp`, `Gui/ViewerTab.cpp`
  - approach: extend M19.P2.T6's Viewer input acceptance and kind-driven view switch to the geometry kind. When the Viewer's active input is geometry, the viewer stage opens that node's `natrongeo` layer (M54.P2.T5), the same layer `GeoToUSD` renders from, so viewing and rendering share one conversion. A detail-hash change reloads the layer in place, so scrubbing and value edits dirty points instead of rebuilding the stage (the 2026-10-25 decision). Packed prims draw as their bounds. Picking (M19.P2.T3) returns the prim path, which maps back to the detail's prim index by table.
  - verify: manual — a `GeoPrimitive` node viewed in the Viewer's 3D view shows the mesh; scrubbing a time-varying upstream node updates it without a stage rebuild (no resync notice); clicking a prim reports its index; a packed `USDToGeo` output shows bounds boxes.
  - size: M

## Phase 54.3: Operators

- [ ] M54.P3.T1 — `GeoPrimitive`
  - files: `Engine/Nodes/Geometry/GeoPrimitive.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoPrimitive_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: generator; type knob `card | grid | cube | sphere | cylinder`, size/segment knobs per type, emits `st` (`faceVarying`) and normals. Card is Nuke's Card: a single quad in XY with optional subdivisions.
  - verify: unit test — each type yields the expected point/face counts and a closed index buffer; UVs cover 0–1.
  - size: M

- [ ] M54.P3.T2 — `GeoImportOBJ`
  - files: `Engine/Nodes/Geometry/GeoImportOBJ.h`/`.cpp`, `Engine/Nodes/Geometry/ObjParser.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoImportOBJ_Test.cpp`, a tiny committed `.obj`
  - approach: minimal OBJ parser (v/vt/vn/f with n-gons, groups → prim groups, objects → prims); USD/Alembic import is `USDRead` → `USDToGeo`, so this node is OBJ only. Missing file sets a node error.
  - verify: unit test — the committed `.obj` loads with matching counts, UVs and two groups.
  - size: M

- [ ] M54.P3.T3 — `GeoTransform`
  - files: `Engine/Nodes/Geometry/GeoTransform.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoTransform_Test.cpp`
  - approach: TRS + pivot knobs (same layout as `USDTransform`), optional group restriction; transforms points and normals (COW copy of those two arrays only, everything else stays shared with the input). Packed prims get their transform multiplied instead; they have no points to move.
  - verify: unit test — transform moves points; `st` storage is identical to the input's (`VtArray::IsIdentical`); a packed prim's transform is multiplied and its bounds move with it.
  - size: M

- [ ] M54.P3.T4 — `GeoMerge`
  - files: `Engine/Nodes/Geometry/GeoMerge.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoMerge_Test.cpp`
  - approach: multi-input concatenation of prims in input order; prim-path collisions get a numeric suffix; groups keep their names per prim.
  - verify: unit test — merge of two details has both prims with unique paths and each input's groups intact.
  - size: M

- [ ] M54.P3.T5 — `GeoGroup`
  - files: `Engine/Nodes/Geometry/GeoGroup.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoGroup_Test.cpp`
  - approach: creates a named point or prim group from one of: a bounding box, a prim-name pattern (`PrimSelector` syntax over prim paths), or an attribute comparison (`attr op value`, scalar attributes). Output is the input with the group added (COW: only the groups map changes).
  - verify: unit test — bbox group of a grid selects exactly the left-half points; a prim pattern selects exactly the matching prim; an attribute comparison over a `vertex` float attribute selects the expected set.
  - size: M

- [ ] M54.P3.T6 — `GeoDelete`
  - files: `Engine/Nodes/Geometry/GeoDelete.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoDelete_Test.cpp`
  - approach: removes the members of a named group, or its complement. Points: drop the points and every face touching them, re-index faces and every `vertex`/`faceVarying` attribute. Prims: drop them. Filtered copy.
  - verify: unit test — deleting a grid's left-half group leaves the right half with a valid, re-indexed buffer and consistent attribute lengths; "delete non-selected" gives the complement.
  - size: M

- [ ] M54.P3.T7 — `GeoNormals`
  - files: `Engine/Nodes/Geometry/GeoNormals.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoNormals_Test.cpp`
  - approach: computes area-weighted vertex normals as a `vertex` `normals` attribute, optionally face normals as a `uniform` attribute; exposes the computation as a static helper other nodes (`GeoDisplace`, `GeoEdit`) call after moving points.
  - verify: unit test — normals of a cube are unit length and outward; a flat grid's are all +Z.
  - size: M

- [ ] M54.P3.T8 — `GeoDisplace`
  - files: `Engine/Nodes/Geometry/GeoDisplace.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoDisplace_Test.cpp`
  - approach: moves points along normals by a scalar from a noise knob or an optional **Image** input sampled at `st` (luminance), with amplitude and offset knobs and optional group restriction; recomputes normals via `GeoNormals`' helper afterwards; computes normals first if the input has none.
  - verify: unit test — a flat grid displaced by a constant image moves every point by the same amount along +Z and its normals stay +Z.
  - size: M

- [ ] M54.P3.T9 — `GeoSubdivide` via OpenSubdiv
  - files: `Engine/Nodes/Geometry/GeoSubdivide.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoSubdivide_Test.cpp`, CMake (link OpenSubdiv, already a USD dependency)
  - approach: Catmull-Clark or Loop, level knob 1–4, interpolates `st` and other `vertex`/`faceVarying` attributes through the refiner; prim groups survive as face subsets. Uses OpenSubdiv's Far refiner directly rather than Hydra.
  - verify: unit test — cube at level 1 has the expected face count and stays closed; `st` is interpolated.
  - size: M

- [ ] M54.P3.T10 — `GeoUVProject`
  - files: `Engine/Nodes/Geometry/GeoUVProject.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoUVProject_Test.cpp`
  - approach: writes `st` from a planar (axis + TRS) or camera projection (inputs: optional Scene for the camera; camera path knob), `faceVarying` interpolation. Pairs with M53's `ProjectTexture` for geometry with no UVs.
  - verify: unit test — planar projection of a grid yields `st` equal to its normalized XY; camera projection of a camera-facing quad yields 0–1.
  - size: M

- [ ] M54.P3.T11 — `GeoWrangle`: Python over the detail's arrays
  - files: `Engine/Nodes/Geometry/GeoWrangle.h`/`.cpp`, `Engine/Nodes/Geometry/GeoPyBuffer.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoWrangle_Test.cpp`
  - approach: a script knob (multi-line `KnobString`) run in Natron's embedded Python under the GIL during `renderGeometry()`, with a `geo` object exposing each prim's arrays via the CPython buffer protocol (a small hand-written extension type, no Shiboken typesystem changes, no pxr Python): `geo.prims[i].points` is a writable float `memoryview` of shape (N,3); attributes likewise by name; `geo.prims[i].group("name")` a bool view. Writes go to the COW-detached output copy. numpy is optional (`np.frombuffer(view)` if present). Script errors become a node error with the traceback.
  - verify: unit test — script `for p in geo.prims: pts = p.points; ...` doubling every Z runs and doubles Z; a syntax error reports on the node without aborting the render thread.
  - size: L

## Phase 54.4: Point editing

- [ ] M54.P4.T1 — 3D-view point picking
  - files: `Gui/SceneViewport.cpp`/`.h`
  - approach: a points pick mode using `UsdImagingGLEngine::TestIntersection` with the pick target set to points (Storm renders point-id AOVs for this), click and marquee select, selection stored as (prim path, point index) set. Highlighting uses Storm's own selection: an `HdSelection` with point-index highlighting through the engine's selection tracker, plus guide prims (purpose `guide`) authored into a viewer-private session layer for point markers and the marquee. Nothing is drawn by a second GL pass outside Hydra. Component display and picking exist **only in the geometry context**, when the Viewer's active input is geometry. In the scene context, picking stays at prim level (M19.P2.T3, M232 - Scene Selection To Nodes), as in Solaris.
  - verify: manual — clicking a vertex of a `GeoPrimitive` cube highlights it; marquee selects the expected set; viewing a `GeoToUSD` downstream of the same cube offers prim picking only, with no point mode.
  - size: M

- [ ] M54.P4.T2 — `GeoEdit` node with viewport drag
  - files: `Engine/Nodes/Geometry/GeoEdit.h`/`.cpp`, `Gui/SceneViewport.cpp` (drag → knob writeback), `Engine/AppManager.cpp`, `Tests/GeoEdit_Test.cpp`
  - approach: stores a sparse map `(prim index, point index) → offset` in a serialized knob (a `KnobString` holding a compact text encoding is acceptable; a table knob if one fits); render applies offsets to points (COW-detaching `points` only) and recomputes normals if present. Viewport: with the node selected and points picked (T1), M20.P2.T1's translate gizmo drags them; release writes one knob change (one undo entry). Topology guard: if the input's point count for a prim differs from the count recorded when the edit was made, skip that prim's edits and set a node warning.
  - verify: unit test — an edit map round-trips through serialization and moves exactly the listed points; changing the upstream point count triggers the warning and leaves points unmodified. Manual — drag creates one undo entry.
  - size: L

## Phase 54.5: Integration

- [ ] M54.P5.T1 — Geometry pipeline integration test in CI
  - files: `Tests/GeoPipeline_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: headless graph — `GeoPrimitive`(grid) → `GeoTransform` → `GeoGroup`(bbox) → `GeoDelete` → `GeoDisplace`(constant) → `GeoNormals` → `GeoEdit`(one point) → `GeoToUSD` → `USDMerge`(with `USDRead`) → `USDWrite`; assert the exported mesh's point count, one edited point's position, and the `primvars:normals`. Plus `USDToGeo`(unpacked) of the same export equals the pre-bridge detail. A second chain, the SOP Modify round trip: `USDRead` → `USDToGeo`(packed) → `GeoTransform` → `GeoToUSD`(Scene input connected) → `USDWrite`; assert the source prims moved at their original paths and the export has no extra prims.
  - verify: test green in `build-and-test`.
  - size: M

**Verification gate:** CI green including M54.P5.T1; on real hardware a geometry chain displays in the Viewer's 3D view through its `natrongeo` layer, scrubbing it dirties points without a stage rebuild, point picking and `GeoEdit` drag work with undo in the geometry context only, and the result renders through `GeoToUSD` → `USDRender` to an image with no copied layer; `GeoWrangle` runs a script that edits points; geometry cache respects its own budget; pre-existing ctest suite green; `Engine/GeoDetail.h` and `Engine/` core stay pxr-free.

## Decisions

- 2026-10-25 — **Long-lived USD scene (bosley-native-nodes, from the review in `PLAN/DESIGN/2026-10-25-usd-long-lived-scene-review.md`):** The scratch stage "keyed by detail hash" would rebuild per frame: use a viewer-private geometry layer edited in place instead, so value-only changes become dirty points. GeoToUSD layer keys follow M19's per-node read keys. Geometry keys on time only when something upstream varies with time (user).
- 2026-10-26 — **USD primitives come first (user, M19/M20 UAT grill):** card, sphere, cube and cylinder ship as USD-authoring source nodes in M227 - USD Primitive Nodes; `GeoPrimitive` (P3.T1) stays the native-geometry generator and should share M227's `PrimitiveMeshes` builders rather than duplicate them. Read `USDTransform` as `USDEditTransform` (M226 - Scene Hierarchy From Topology).
- 2026-10-27 — **Bridges follow Solaris (user, from a Houdini SOP Import/SOP Modify investigation; `PLAN/DECISIONS/2026-10-27-geometry-bridges-follow-solaris.md`):** `GeoToUSD` serves the detail through a `natrongeo` Sdf file format plugin (new M54.P2.T5) rather than authoring into a layer, and copying is the "Copy Into Layer" opt-in. `USDToGeo` brings prims in packed by default with Unpack on request, and `GeoToUSD` writes the result back as overrides on the source paths (new M54.P2.T6). Component display and picking exist only in the geometry context, through `HdSelection` highlighting plus guide prims. The plugin layer is identified by node, not content hash (user): a hash change reloads it in place, which keeps the 2026-10-25 dirty-points behaviour, and the viewer-private layer in that decision is now this layer. The scratch stage and `GeoScratchStage` are gone.
