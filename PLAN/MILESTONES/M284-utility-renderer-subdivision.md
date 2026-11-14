# M284 - Utility Renderer Subdivision

Refine subdivision meshes in Natron Utility with OpenSubdiv: Catmull-Clark, Loop and bilinear
schemes, creases, corners, holes, boundary and face-varying interpolation, uniformly refined to
a level set by a render setting and overridable per prim. Refinement is cached per topology
and level; deforming meshes re-evaluate points through the cached stencil tables, which keeps
motion blur cheap.

## Scope

**In:**
- `Subdivision Level` render setting (default 2, range 0–6) applying to every mesh whose scheme is not `none`; primvar `natron:subdivLevel` (int, constant) overrides per prim. The viewer still passes Hydra's `refineLevel` (display complexity) when no override is set in the viewer's own settings.
- OpenSubdiv `Far` uniform refinement from `HdMeshTopology` and `PxOsdSubdivTags`: creases, corners, holes, `interpolateBoundary`, `faceVaryingLinearInterpolation`, `triangleSubdivisionRule`.
- Primvars refined by interpolation type (vertex, varying, face-varying with its own channel, uniform carried per refined face).
- Limit normals from the limit surface.
- Stencil-table cache keyed by (topology hash, level, tags); point and motion-sample updates re-apply the stencils without re-refining.
- Same refined triangles feed both backends.

**Out:**
- Screen-space adaptive dicing — M289 - Utility Adaptive Subdivision (user, 2026-11-01).
- Displacement — M287 - Utility Renderer Displacement.
- GPU-side refinement (OSD GPU kernels) — none; refinement and stencil evaluation run on CPU and upload.

**Edge cases:**
- Level 0: the cage is triangulated as in M279 - Utility Renderer Foundation, with authored normals or flat ones.
- Non-manifold topology OpenSubdiv rejects: render the cage, warn naming the prim.
- Level × face count above a triangle budget (setting `Max Subdivided Triangles`, default 50M): the level is lowered for that prim until it fits, with a warning.
- Topology changes within the shutter: handled by M283's static fallback.
- `scheme = none` with a level override: no refinement.

**Assumptions:**
- The pinned USD links OpenSubdiv (cmake/NatronUsd.cmake redirects `OpenSubdiv_DIR`) and its `Far` headers are installed for direct use.

## Phase 284.1: Refinement

- [x] M284.P1.T1 — OpenSubdiv refiner and stencil cache
  - files: `Engine/Nodes/Scene/Utility/UtilitySubdiv.{h,cpp}`, `Engine/Nodes/Scene/Utility/CMakeLists.txt`, `Tests/Scene/NatronUtilitySubdiv_Test.cpp`
  - approach: build a `Far::TopologyRefiner` from `HdMeshTopology` + `PxOsdSubdivTags` (cite pxOsd's conversion helpers in the pinned USD and reuse them if exposed), refine uniformly, build vertex, varying and face-varying stencil tables and the refined face list; cache by (topology hash, level, tags) with an LRU budget.
  - verify: gtest — a cube refined at level 1 has 24 quads (48 triangles) whose points match OpenSubdiv's reference; a crease of sharpness 10 keeps its edge on the limit; a hole face produces no triangles.
  - size: L

- [x] M284.P1.T2 — Refined meshes in sync and tracing
  - files: `Engine/Nodes/Scene/Utility/UtilityMesh.cpp`, `Engine/Nodes/Scene/Utility/UtilityScene.cpp`, `Engine/Nodes/Scene/Utility/UtilityRenderDelegate.cpp`, `Tests/Scene/NatronUtilitySubdiv_Test.cpp`
  - approach: subdiv meshes produce refined triangles, refined primvars and limit normals through the stencils; point edits and per-bucket motion samples re-apply stencils only. Register `Subdivision Level` and `Max Subdivided Triangles`; read the `natron:subdivLevel` primvar.
  - verify: gtest — a Catmull-Clark sphere cage renders silhouette error below 0.5 px at level 3 against an analytic sphere; editing a cage point re-runs stencils without a new refiner (counter); the primvar override beats the setting; the budget lowers the level and warns.
  - size: L

- [ ] M284.P1.T3 — Face-varying UVs and render check
  - files: `Engine/Nodes/Scene/Utility/UtilitySubdiv.cpp`, `Tests/Scene/NatronUtilitySubdiv_Test.cpp`, `Tests/fixtures/native-golden/NatronUtility/`
  - approach: refine face-varying `st` with its own channel honouring `faceVaryingLinearInterpolation`; record a golden of a textured subdiv model.
  - verify: gtest — a UV seam stays continuous across refinement per the interpolation rule; the golden matches on both backends.
  - size: M

**Verification gate:** full build and ctest green on both backends; a subdivided, motion-blurred test render is shared with the user.

## Decisions

- 2026-11-01 — **Level from a setting plus a per-prim primvar (user);** screen-space adaptive dicing is a follow-up, M289 - Utility Adaptive Subdivision.
- 2026-11-14 — Claimed by bosley-native-nodes (user request).
- 2026-11-14 — P1.T1 and P1.T2 implemented, plus T3's face-varying channels (925008b36). T3's render golden waits on M281's texturing. PM calls: subdivision level precedence is the primvar, then the viewer's refineLevel (when above 0), then the setting. Refinement must be cancellable (renders cancel fast); a follow-up fix is in progress. Older tests are pinned to the cage, since the default level is now 2.
