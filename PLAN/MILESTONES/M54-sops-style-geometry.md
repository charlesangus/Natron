# Milestone 54: SOPs-style geometry — GeoDetail data kind, bridges, operators, point editing

Stage 3 of the 3D roadmap (`PLAN/DESIGN/2026-09-18-3d-roadmap-lops-paint-sops.md`,
Question 3). A fourth data kind, `eDataKindGeometry`, carrying a
copy-on-write `GeoDetail` (UsdGeom's attribute vocabulary without a
stage); explicit bridges to and from the scene stack; the same
`Viewport3D` for display via a scratch stage; a Nuke-classic-sized
operator set plus a Python wrangle; and EditGeo-style point editing.
Decision: `PLAN/DECISIONS/2026-09-18-geometry-is-a-fourth-data-kind.md`.
Requires M52 (`PrimSelector`) and M20.P2.T1 (gizmo). Runs after M53.
All nodes are `NativeEffectBase` subclasses in `Engine/Nodes/Geometry/`,
registered under `PLUGIN_GROUP_3D`; mirror M18's deep plumbing wherever a
choice arises.

## Phase 54.1: Data kind, container, cache, pull path

- [ ] M54.P1.T1 — `eDataKindGeometry` declared and drawn
  - files: `Global/Enums.h`, `Gui/Edge.cpp`, `Gui/NodeGui.cpp`, `Tests/DataKind_Test.cpp`, `Engine/Nodes/README.md`
  - approach: add the fifth `DataKindEnum` value; edge styling and node silhouette per the 2026-09-05 design doc's "Communicating kind" rules (distinct from image/deep/scene); connection typing needs no change — it is kind-agnostic. Document the kind in the README's "Data kinds" section with the same "declaration only until 54.1 lands" caveat deep carried.
  - verify: `DataKind_Test` extended — a geometry source into a scene input is refused, geometry → `Dot` → geometry consumer resolves; Xvfb GUI screenshot shows the new edge style.
  - size: S

- [ ] M54.P1.T2 — `GeoDetail` value type
  - files: `Engine/GeoDetail.h`/`.cpp` (pimpl, no pxr header), `Engine/Nodes/Geometry/GeoDetailImpl.h`/`.cpp`, `Tests/GeoDetail_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: per the design doc's container sketch — `prims` (kind mesh/points/curves, path token, `VtArray` points/counts/indices, attribute map keyed by (interpolation, name) → `VtValue`, groups name → bitset), `contentHash` (U64 over all arrays, computed lazily and cached), `getSizeInBytes()` aliasing-blind like `DeepImage`. Mutation API returns a new detail sharing untouched arrays (`VtArray` COW). Public header exposes typed accessors (`const float*`/counts) so `Engine/` core and tests never see pxr.
  - verify: unit tests — copying then mutating `points` detaches only `points` (other arrays share storage, checked via `VtArray::IsIdentical`); identical content hashes identically; `Engine/GeoDetail.h` includes no pxr header (grep-enforced, as M19.P1.T2).
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
  - approach: both directions: detail prim ↔ `UsdGeomMesh`/`UsdGeomPoints`/`UsdGeomBasisCurves`, attributes ↔ primvars with interpolation, groups ↔ `UsdGeomSubset` (prim groups) or a `bool[]` primvar (point groups). Operates on an `SdfLayer`/`UsdPrim` at a given `UsdTimeCode`; no node logic here.
  - verify: round-trip unit test on an anonymous layer — a detail with a mesh, a `vertex` color attribute and a prim group authors a `UsdGeomMesh` with matching points/indices, a `primvars:color` with `vertex` interpolation and a `UsdGeomSubset`; converting back yields an identical detail (same content hash).
  - size: M

- [ ] M54.P2.T2 — `GeoToScene`
  - files: `Engine/Nodes/Geometry/GeoToScene.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoToScene_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: inputs Geometry (+ optional Scene to append to); knobs: root scope path, "author time samples across range" (default off — vertex data is never auto-baked). Output kind **scene**: authors the prims into the node's layer via `SceneOps` and `GeoUsdConvert` at the current time; memoized per M19.P1.T3 keyed on the detail hash.
  - verify: unit test — a `GeoConstant` detail appears under the scope path on the composed stage; re-evaluating with an unchanged detail re-emits the same layer handle.
  - size: M

- [ ] M54.P2.T3 — `SceneToGeo`
  - files: `Engine/Nodes/Geometry/SceneToGeo.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/SceneToGeo_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: inputs Scene; knobs: `PrimSelector`, "flatten transforms" (bake world xform into points), time follows the frame. Uses `GeoUsdConvert` over the composed input stage. Prims of unsupported types are skipped and listed in the node's info message.
  - verify: unit test — `ReadScene`(reference `.usda`) → `SceneToGeo` yields the expected mesh; with flatten on, a translated prim's points move; `GeoToScene` → `SceneToGeo` is the identity on a detail.
  - size: M

- [ ] M54.P2.T4 — `Viewport3D` displays geometry nodes through a scratch stage
  - files: `Gui/Viewport3D.cpp`/`.h`, `Engine/Nodes/Geometry/GeoScratchStage.h`/`.cpp`
  - approach: when the viewed node's effective kind is geometry, pull its detail, convert via `GeoUsdConvert` into an anonymous layer under `/Natron/geo`, and hand the resulting stack to the existing viewport path (`StageCache` keyed by detail hash, so unchanged geometry costs nothing). Picking (M19.P2.T3) returns the prim path, which maps back to the detail's prim index by table.
  - verify: manual — a `GeoPrimitive` node viewed in `Viewport3D` shows the mesh; scrubbing a time-varying upstream node updates it; clicking a prim reports its index.
  - size: M

## Phase 54.3: Operators

- [ ] M54.P3.T1 — `GeoPrimitive`
  - files: `Engine/Nodes/Geometry/GeoPrimitive.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoPrimitive_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: generator; type knob `card | grid | cube | sphere | cylinder`, size/segment knobs per type, emits `st` (`faceVarying`) and normals. Card is Nuke's Card: a single quad in XY with optional subdivisions.
  - verify: unit test — each type yields the expected point/face counts and a closed index buffer; UVs cover 0–1.
  - size: M

- [ ] M54.P3.T2 — `GeoImportOBJ`
  - files: `Engine/Nodes/Geometry/GeoImportOBJ.h`/`.cpp`, `Engine/Nodes/Geometry/ObjParser.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoImportOBJ_Test.cpp`, a tiny committed `.obj`
  - approach: minimal OBJ parser (v/vt/vn/f with n-gons, groups → prim groups, objects → prims); USD/Alembic import is `ReadScene` → `SceneToGeo`, so this node is OBJ only. Missing file sets a node error.
  - verify: unit test — the committed `.obj` loads with matching counts, UVs and two groups.
  - size: M

- [ ] M54.P3.T3 — `GeoTransform`
  - files: `Engine/Nodes/Geometry/GeoTransform.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoTransform_Test.cpp`
  - approach: TRS + pivot knobs (same layout as `Transform3D`), optional group restriction; transforms points and normals (COW copy of those two arrays only, everything else stays shared with the input).
  - verify: unit test — transform moves points; `st` storage is identical to the input's (`VtArray::IsIdentical`).
  - size: S

- [ ] M54.P3.T4 — `GeoMerge`
  - files: `Engine/Nodes/Geometry/GeoMerge.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoMerge_Test.cpp`
  - approach: multi-input concatenation of prims in input order; prim-path collisions get a numeric suffix; groups keep their names per prim.
  - verify: unit test — merge of two details has both prims with unique paths and each input's groups intact.
  - size: S

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
  - size: S

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
  - size: S

- [ ] M54.P3.T11 — `GeoWrangle`: Python over the detail's arrays
  - files: `Engine/Nodes/Geometry/GeoWrangle.h`/`.cpp`, `Engine/Nodes/Geometry/GeoPyBuffer.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoWrangle_Test.cpp`
  - approach: a script knob (multi-line `KnobString`) run in Natron's embedded Python under the GIL during `renderGeometry()`, with a `geo` object exposing each prim's arrays via the CPython buffer protocol (a small hand-written extension type, no Shiboken typesystem changes, no pxr Python): `geo.prims[i].points` is a writable float `memoryview` of shape (N,3); attributes likewise by name; `geo.prims[i].group("name")` a bool view. Writes go to the COW-detached output copy. numpy is optional (`np.frombuffer(view)` if present). Script errors become a node error with the traceback.
  - verify: unit test — script `for p in geo.prims: pts = p.points; ...` doubling every Z runs and doubles Z; a syntax error reports on the node without aborting the render thread.
  - size: L

## Phase 54.4: Point editing

- [ ] M54.P4.T1 — `Viewport3D` point picking
  - files: `Gui/Viewport3D.cpp`/`.h`
  - approach: a points pick mode using `UsdImagingGLEngine::TestIntersection` with the pick target set to points (Storm renders point-id AOVs for this), click and marquee select, selection stored as (prim path, point index) set and drawn as highlighted points. Available only when the viewed node's kind is geometry.
  - verify: manual — clicking a vertex of a `GeoPrimitive` cube highlights it; marquee selects the expected set.
  - size: M

- [ ] M54.P4.T2 — `GeoEdit` node with viewport drag
  - files: `Engine/Nodes/Geometry/GeoEdit.h`/`.cpp`, `Gui/Viewport3D.cpp` (drag → knob writeback), `Engine/AppManager.cpp`, `Tests/GeoEdit_Test.cpp`
  - approach: stores a sparse map `(prim index, point index) → offset` in a serialized knob (a `KnobString` holding a compact text encoding is acceptable; a table knob if one fits); render applies offsets to points (COW-detaching `points` only) and recomputes normals if present. Viewport: with the node selected and points picked (T1), M20.P2.T1's translate gizmo drags them; release writes one knob change (one undo entry). Topology guard: if the input's point count for a prim differs from the count recorded when the edit was made, skip that prim's edits and set a node warning.
  - verify: unit test — an edit map round-trips through serialization and moves exactly the listed points; changing the upstream point count triggers the warning and leaves points unmodified. Manual — drag creates one undo entry.
  - size: L

## Phase 54.5: Integration

- [ ] M54.P5.T1 — Geometry pipeline integration test in CI
  - files: `Tests/GeoPipeline_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: headless graph — `GeoPrimitive`(grid) → `GeoTransform` → `GeoGroup`(bbox) → `GeoDelete` → `GeoDisplace`(constant) → `GeoNormals` → `GeoEdit`(one point) → `GeoToScene` → `Merge3D`(with `ReadScene`) → `WriteScene`; assert the exported mesh's point count, one edited point's position, and the `primvars:normals`. Plus `SceneToGeo` of the same export equals the pre-bridge detail.
  - verify: test green in `build-and-test`.
  - size: S

**Verification gate:** CI green including M54.P5.T1; on real hardware a geometry chain displays in `Viewport3D`, point picking and `GeoEdit` drag work with undo, and the result renders through `GeoToScene` → `HydraRender` to an image; `GeoWrangle` runs a script that edits points; geometry cache respects its own budget; pre-existing ctest suite green; `Engine/GeoDetail.h` and `Engine/` core stay pxr-free.
