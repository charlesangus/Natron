# M149 - SplineWarp Mesh Solver

Add a second, user-selectable SplineWarp solver: a constrained Delaunay triangulation (CDT) over the destination samples, with the source position interpolated barycentrically inside each triangle. It is local and predictable where TPS is smooth and global. It feeds the same coordinate field, boundary weighting and render path as M147 - SplineWarp Node Core's TPS, so everything downstream (GPU, motion blur) works unchanged.

Deps: M147 - SplineWarp Node Core (sampling, the field, the solver knob).

Scouted facts (2026-10-20, `main` at `8fdb5741f`):
- There is no Delaunay or CDT code in the tree. `libs/libtess` (`Engine/RotoContext.cpp:42,3971`, linked as `tess`) is a GLU polygon tessellator, not a CDT.
- Vendored libraries live under `libs/<name>/` with their own `CMakeLists.txt` (e.g. `libs/ceres`, `libs/libtess`).

## Scope

**In:**
- Vendored `artem-ogre/CDT` (header-only, MPL-2.0) at `libs/CDT`.
- A mesh built from M147 - SplineWarp Node Core's sample pairs:
  - destination positions as vertices;
  - constrained edges along each destination polyline and each boundary feather polyline;
  - the corners of the padded render RoD pinned at zero displacement.
- Rasterizing the mesh into the shared coordinate field at the field step (barycentric source coordinates). The boundary weight and feather ramp are applied as for TPS.
- A `Mesh` option on the solver knob. `smoothness` is hidden when Mesh is selected.

**Out:**
- A mesh wireframe overlay → none (user: no field/mesh preview, 2026-10-20).
- The GPU path → M150 - SplineWarp GPU Kernel. It samples the field, so it needs nothing mesh-specific.

**Edge cases:**
- Fewer than three non-collinear vertices: fall back to the TPS fallbacks (translation, similarity, affine).
- Intersecting constraint edges (crossing destination curves): CDT's intersection-resolving mode splits them at the crossing. The new vertex's source position is interpolated along the first edge.
- Coincident vertices: merged by M147 - SplineWarp Node Core's 1e-3 px rule before triangulation.
- A source-space fold (a triangle whose source image is inverted): rendered as is, consistent with TPS's no-fold-guard rule.
- Field pixels outside every triangle (beyond the padded corners): identity.

**Assumptions:**
- `artem-ogre/CDT`'s licence (MPL-2.0) is acceptable alongside Natron's GPL. Confirm at promotion, and record it in the vendoring commit.

## Phase 149.1: Mesh solver

- [ ] M149.P1.T1 — Vendor CDT
  - files: `libs/CDT/` (new: headers, `LICENSE`, `CMakeLists.txt` interface target), `libs/CMakeLists.txt` or the top-level `CMakeLists.txt` where libs are added, `Engine/CMakeLists.txt`
  - approach: add the pinned upstream release as a header-only interface target, link it into Engine, and note the version and licence in the libs README if one exists.
  - verify: the build passes. A one-line include in P1.T2's test compiles.
  - size: M

- [ ] M149.P1.T2 — Build the constrained mesh from sample pairs
  - files: `Engine/Nodes/Transform/SplineWarp/SplineWarpMesh.{h,cpp}` (new), `Tests/Native/SplineWarpMesh_Test.cpp` (new)
  - approach:
    - Collect the vertices: destination sample positions, with their source positions as values, and the padded RoD corners as zero-displacement vertices.
    - Constrain the edges between consecutive samples of each destination polyline and each boundary feather polyline, and triangulate with intersection resolution.
    - Return the triangles with per-vertex source positions, cached by the sample-set hash.
  - verify: the test covers the triangle count and validity for a square of samples, that the constrained edges are present, the crossing-curve split, and the fallback below three vertices.
  - size: L

- [ ] M149.P1.T3 — Rasterize the mesh into the coordinate field and add the solver option
  - files: `Engine/Nodes/Transform/SplineWarp/SplineWarpMesh.cpp`, `Engine/Nodes/Transform/SplineWarp/SplineWarpField.cpp`, `Engine/Nodes/Transform/SplineWarp/SplineWarp.cpp`, `Tests/Native/SplineWarpMesh_Test.cpp`
  - approach:
    - Scan-convert each triangle over the field grid, writing barycentric source coordinates. Points outside every triangle are identity.
    - Apply the shared boundary weight afterwards.
    - Add `Mesh` to the solver knob, hide `smoothness` for Mesh, and include the solver in the field cache key.
  - verify: the test covers a translation link giving a uniform field, the field matching the source exactly at the vertices (1e-6), affine samples reproducing the affine everywhere inside the hull (1e-5), and a SplineWarp render with Mesh matching TPS on a pure-affine fixture (1e-4).
  - size: M

**Verification gate:**
- The build and the full ctest pass, including `SplineWarpMesh_Test`.
- Xvfb GUI check, with screenshots shared before sign-off: the same curve setup rendered with TPS and with Mesh, side by side.
- An AppImage built with `package.sh` for UAT.

## Decisions

- 2026-10-28: Implemented (build pending). CDT 2.0.1 vendored header-only in libs/CDT (MPL-2.0; predicates.h BSD-3; portable_nth_element Apache-2.0 w/ LLVM exception — all compatible with Natron's GPLv2-or-later distributed as GPLv3), excluded from the format gate. Mesh box = Source full-size bounds plus all samples, padded by ¼ of the larger side (≥1 px); in the cache key and warp hash. <3 distinct or collinear destinations → TPS fallback. Crossing curves: CDT inserts a vertex taking the source of the first-inserted curve; unresolvable → unconstrained mesh → TPS. Outside the box the field is identity; folded triangles rendered as-is. Sample hash includes curve order.
- 2026-10-20 — **Both solvers, user-selectable (user):** this milestone adds the mesh option.
- 2026-10-20 — **CDT feeds the shared field (planner):** the GPU, motion blur and render paths stay solver-agnostic.
