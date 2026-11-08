# M361 - Geometry Point Editing

Last code member of F15 - Native Geometry. Point and primitive picking in the Viewer's 3D view,
in the geometry context only, and `GeoEdit`, Houdini's Edit SOP: drag points or packed
primitives with the translate gizmo, keyed by global point number. Also the Feature's
headless integration test and the scale bench.
Shared design: M54 - Native Geometry Data Model's intro and Scope, `PLAN/DECISIONS/2026-11-08-native-geometry-follows-sops.md`, `PLAN/DECISIONS/2026-10-27-geometry-bridges-follow-solaris.md`.

Member of F15 - Native Geometry: branches off and merges into `feature/f15-native-geometry` (PLAN-FORMAT §9b); the Feature's UAT covers it.

## Scope

**In:**
- Point and primitive pick modes with click and marquee, highlighted through `HdSelection` plus guide prims, only when the Viewer's active input is geometry.
- `GeoEdit` keyed by global point number, moving packed primitives by their transforms, with one undo entry per drag.
- The geometry pipeline integration test and the 10M-point GPU-host bench.

**Out:**
- Component selection or editing in the scene context (M232 - Scene Selection To Nodes).
- Topology-changing interactive tools (none).
- Bench timing or memory thresholds (none).

**Edge cases:**
- The upstream point or primitive count changes under a `GeoEdit`: it skips every edit and sets a warning.
- Packed primitives pick and edit as whole primitives.

Requires M358 - Geometry Bridges And Display, M359 - Geometry Operators and M360 - Geometry Kernel Language (the integration test uses their nodes), and F4 - Heavy Scene Viewer landed, through M336 - Heavy Scene Acceptance. Gizmo from M20.P2.T1. All nodes are `NativeEffectBase` subclasses in `Engine/Nodes/Geometry/`, each registered through its own `.reg.inc` (the pattern in `Engine/Nodes/Scene/USDCard.reg.inc`), under `PLUGIN_GROUP_3D`. Native geometry uses SOP attribute names (`P`, `N`, `uv`, `Cd`, `pscale`, `orient`, `path`, `name`); only `GeoUsdConvert` maps them to USD names.

## Phase 361.1: Picking and editing

- [ ] M361.P1.T1 — 3D-view point and primitive picking
  - files: `Gui/SceneViewport.cpp`/`.h`
  - approach: point and primitive pick modes using `UsdImagingGLEngine::TestIntersection` with the pick target set to points or faces (Storm renders the id AOVs for this), with click and marquee select. Selection is stored as global point numbers or primitive numbers, mapped from (prim path, index) through the partition table. Highlighting uses Storm's own selection: an `HdSelection` with point- or element-index highlighting through the engine's selection tracker, plus guide prims (purpose `guide`) in the viewer-private session layer for point markers and the marquee. Nothing is drawn by a second GL pass outside Hydra. Component display and picking exist **only in the geometry context**, when the Viewer's active input is geometry. In the scene context, picking stays at prim level (M19.P2.T3, M232 - Scene Selection To Nodes), as in Solaris. Packed primitives pick as whole primitives.
  - verify: manual — clicking a vertex of a `GeoPrimitive` cube highlights it; marquee selects the expected set; a packed `USDToGeo` output picks whole packed primitives; viewing a `GeoToUSD` downstream of the same cube offers prim picking only, with no point mode.
  - size: M

- [ ] M361.P1.T2 — `GeoEdit` node with viewport drag
  - files: `Engine/Nodes/Geometry/GeoEdit.h`/`.cpp`/`.reg.inc`, `Gui/SceneViewport.cpp` (drag → knob writeback), `Tests/GeoEdit_Test.cpp`
  - approach: the Edit SOP. It stores a sparse map from global point number to offset, and from packed primitive number to transform delta, in a serialized knob (a `KnobString` holding a compact text encoding is acceptable; a table knob if one fits). It also records the input's point and primitive counts at edit time. Render applies the offsets to `P` (detaching only the touched pages) and multiplies the packed primitives' transforms. It then recomputes `N` if present. In the viewport, with the node selected and points or packed primitives picked (M361.P1.T1), M20.P2.T1's translate gizmo drags them, and release writes one knob change (one undo entry). Topology guard: if the input's point or primitive count differs from the recorded count, skip all edits and set a node warning.
  - verify: unit test — an edit map round-trips through serialization and moves exactly the listed points; a packed primitive's edit moves its transform only; changing the upstream point count triggers the warning and leaves points unmodified. Manual — drag creates one undo entry.
  - size: L

## Phase 361.2: Integration and scale

- [ ] M361.P2.T1 — Geometry pipeline integration test in CI
  - files: `Tests/GeoPipeline_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: headless graph — `GeoPrimitive`(grid) → `GeoTransform` → `GeoGroup`(bbox) → `GeoDelete` → `GeoDisplace`(constant) → `GeoNormals` → `GeoWrangle`(`@P.y += 0.1`) → `GeoEdit`(one point) → `GeoToUSD` → `USDMerge`(with `USDRead`) → `USDWrite`. Assert the exported mesh's point count, one edited point's position, and `normals`. Also `USDToGeo`(unpacked) of the same export equals the pre-bridge detail after defragment. A second chain is the SOP Modify round trip: `USDRead` → `USDToGeo`(packed) → `GeoTransform` → `GeoToUSD`(Scene input connected) → `USDWrite`. Assert the source prims moved at their original paths and the export has no extra prims. A third is scatter-instancing: `GeoScatter` → `GeoCopyToPoints`(Pack and Instance) → `GeoToUSD` → `USDWrite`, asserting one `PointInstancer` with the scattered count.
  - verify: test green in `build-and-test`.
  - size: M

- [ ] M361.P2.T2 — Scale bench on the GPU host
  - files: `tools/bench/geo_scale.py` (or the existing bench harness's equivalent), `Tests/GeoBench_Test.cpp` (bench-only, not in ctest)
  - approach: build a 10M-point grid and time `GeoTransform` → `GeoEdit`(5 points) → `GeoDelete`(half) → `GeoToUSD`, before and after, in one command on the GPU host (benches run together there). Record wall time and peak memory, with no thresholds.
  - verify: the bench runs to completion on the GPU host and its numbers are recorded in the member's PR body.
  - size: M

**Verification gate:** CI green including M361.P2.T1; on the Feature branch build under Xvfb, point picking and `GeoEdit` drag work with one undo entry per drag in the geometry context only, and a downstream `GeoToUSD` offers prim picking only; the M361.P2.T2 bench numbers are recorded in the PR body; the pre-existing ctest suite is green.

## Decisions

- 2026-11-08 — Split out of M54 - SOPs-Style Geometry (user, /cat-plan); tasks renumbered from M54.P4.*, M54.P5.* and M54.P1.T8. Waits on F4 - Heavy Scene Viewer landing (user, M54 grill). GeoEdit keys on global point number (user, M54 grill).
