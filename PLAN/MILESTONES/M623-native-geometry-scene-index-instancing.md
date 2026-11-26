# M623 - Native Geometry Scene Index Instancing

Member of F41 - Native Geometry Scene Index. Extend GeoSceneIndex (M622 - Native Geometry Scene Index Core) to every instancing form GeoUsdConvert emits. Renderer plugins see data after UsdImaging's own chain, so the scene index must do UsdImaging's PointInstancer and native-instance jobs itself. Background: `PLAN/DESIGN/2026-11-26-m617-research/bypass-feasibility.md` §1 (Packed / instancing row) and §2.

## Scope

**In:**
- PointInstancer partitions become a Hydra instancer prim with `instancerTopology` and instance primvars (positions, orientations, scales, protoIndices), and prototype prims with `instancedBy`.
- Native instances (`instanceable` with internal references) are synthesised as Hydra instancers.
- Packed USD prims (an `over` on the source path) draw as they do in a geometry view today: bounds guides only. Those guides are M625 - Native Geometry Viewer Stage Readers' to move.
- Instancing fixtures in the parity oracle.

**Out:**
- Packed instances in viewer Cycles: M616 - Packed Instances In Viewer Cycles. Cycles and MoonRay viewers keep the staged path.

**Edge cases:**
- Zero instances gives an instancer with empty topology, not a missing prim.
- A protoIndices-only change dirties only the instance primvar, not the prototypes.

## Phase 623.1: Instancing

- [ ] M623.P1.T1 — Point instancers as Hydra instancers
  - files: `Engine/Nodes/Geometry/GeoSceneIndex.h/.cpp`, `Tests/Scene/GeoSceneIndex_Test.cpp`
  - approach: map GeoUsdConvert's PointInstancer partitions and Prototypes children (`GeoUsdConvert.cpp` ~1170-1196, 1477-1530) to an instancer prim (`HdInstancerTopologySchema`, instance-rate primvars) plus prototype prims carrying `HdInstancedBySchema`. Extend M622.P1.T2's diffing, so an instance-transform change dirties only the instancer primvars.
  - verify: GeoSceneIndex_Test checks the instancer topology, the instance primvars and instancedBy for a copy-to-points detail with two prototypes. A transform-only change dirties only the instancer's primvars.
  - size: L
- [ ] M623.P1.T2 — Native instances as synthesised instancers
  - files: `Engine/Nodes/Geometry/GeoSceneIndex.h/.cpp`, `Tests/Scene/GeoSceneIndex_Test.cpp`
  - approach: for GeoUsdConvert's native-instance mode (`instanceable` + internal references, ~1511-1513), emit one prototype per distinct source and one instancer whose instance transforms are the packed prims' transforms. This is the job UsdImaging's native-instancing propagation does today. Packed USD prims emit no geometry.
  - verify: GeoSceneIndex_Test checks that a packed native detail with three instances of one prototype gives one prototype and one instancer with three transforms.
  - size: L
- [ ] M623.P1.T3 — Instancing fixtures in the parity oracle
  - files: `Tests/Scene/GeoSceneIndexParity_Test.cpp`
  - approach: add the M623.P1.T1 and T2 fixtures. Compare flattened per-instance world transforms and prototype geometry, not raw prim layout, because UsdImaging's propagated prototypes use different paths.
  - verify: the parity test passes for both instancing modes.
  - size: M

**Verification gate:** GeoSceneIndex_Test and GeoSceneIndexParity_Test pass in ctest, including the instancing fixtures. PR CI is green against the F41 - Native Geometry Scene Index branch.

## Decisions
