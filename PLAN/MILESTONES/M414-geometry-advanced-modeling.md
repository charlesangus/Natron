# M414 - Geometry Advanced Modeling

The three heavy SOP modeling operators, written in-house on `GeoDetail` (user, 2026-11-12) so every attribute class and group survives as in Houdini, with no mesh-library conversion: `GeoPolyBevel`, `GeoPolyReduce` (quadric error) and `GeoRemesh` (isotropic). All three need edge adjacency, so this milestone adds a half-edge view over `GeoReverseTopology` that they share.

Member of F21 - Geometry Operators: branches off and merges into `feature/f21-geometry-operators` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E9 - USD And 3D.

## Scope

**In:**
- `GeoHalfEdge`: a lazily built, cached half-edge view (twin, next, boundary flags) over polygon prims, derived from the detail, never stored in it.
- `GeoPolyBevel`: bevel edges or points (by group, or all), offset distance with clamping against overlaps, divisions, profile shape (round/linear/custom ramp), and output groups for the bevel faces.
- `GeoPolyReduce`: quadric-error decimation to a target percentage or count; attribute-aware cost (`uv`, `N`, `Cd` seams), boundary and group locks, keep quads where possible (triangle-pair re-pairing).
- `GeoRemesh`: isotropic remeshing to a target edge length (split, collapse, flip, tangential relaxation), with hard-edge and boundary preservation, and attribute re-sampling from the source surface.

**Out:**
- PolyExtrude, Boolean, Fuse (M362 - Geometry Operators II).
- Quad remeshing / retopology (none).
- Library-based variants (user chose in-house).

**Edge cases:**
- Non-manifold input: bevel and remesh error with a pointer to `GeoClean` (M413 - Geometry Modeling Ops); reduce skips non-manifold edges.
- Bevel offsets larger than adjacent edges: clamped so faces don't invert, with a "clamp overlap" toggle.
- Reduce target below what locks allow: stops at the lowest legal count with an info message.
- Vertex attributes with seams: split vertices are preserved as seams in all three.

**Assumptions:**
- Remesh's closest-point projection and attribute re-sampling use `GeoBVH` from M415 - Geometry Attribute Ops, which this milestone depends on.

## Phase 414.1: Adjacency

- [ ] M414.P1.T1 — `GeoHalfEdge` cached half-edge view
  - files: `Engine/Geo/GeoHalfEdge.{h,cpp}`, `Tests/GeoHalfEdge_Test.cpp`
  - approach: Build twin/next/prev and boundary flags for polygon prims from vertex lists, keyed in the detail's derived-data cache by topology hash like `GeoReverseTopology`. Non-manifold edges flagged, not asserted.
  - verify: `GeoHalfEdge_Test`: a cube has 24 half-edges all twinned; a grid's boundary half-edges are flagged; a non-manifold fan is detected.
  - size: M

## Phase 414.2: Operators

- [ ] M414.P2.T1 — `GeoPolyReduce`
  - files: `Engine/Nodes/Geometry/GeoPolyReduce.{h,cpp,reg.inc}`, `Engine/Geo/GeoQuadric.{h,cpp}`, `Tests/GeoPolyReduce_Test.cpp`
  - approach: Garland–Heckbert quadrics with attribute extension, edge-collapse priority queue, link-condition checks, boundary/group locks, then optional quad re-pairing. Attributes interpolated at the collapse position.
  - verify: `GeoPolyReduce_Test`: a 20k-face sphere reduced to 10 % stays within 1 % of the radius; `uv` seams survive; locked groups keep their points.
  - size: L
- [ ] M414.P2.T2 — `GeoPolyBevel`
  - files: `Engine/Nodes/Geometry/GeoPolyBevel.{h,cpp,reg.inc}`, `Tests/GeoPolyBevel_Test.cpp`
  - approach: Edge and point bevel over the half-edge view: offset new vertices along adjacent faces, build bevel strips with N divisions on the chosen profile, patch corners, clamp offsets against neighbouring edge lengths; output groups.
  - verify: `GeoPolyBevel_Test`: bevelling all edges of a unit cube by 0.1 with 1 division gives 26 faces and the expected bounds; round profile with 4 divisions lies on the arc; clamped overlaps don't invert.
  - size: L
- [ ] M414.P2.T3 — `GeoRemesh`
  - files: `Engine/Nodes/Geometry/GeoRemesh.{h,cpp,reg.inc}`, `Tests/GeoRemesh_Test.cpp`
  - approach: Botsch–Kobbelt isotropic remeshing iterations (split long, collapse short, flip to valence 6, tangential relaxation, project to source), hard edges by angle or group preserved; re-sample attributes from the source by closest point through `GeoBVH` (M415 - Geometry Attribute Ops).
  - verify: `GeoRemesh_Test`: a stretched-triangle plane remeshed to length L has mean edge length within 10 % of L and minimum angle above 30°; a cube with hard edges keeps its corners.
  - size: L

## Phase 414.3: Proof

- [ ] M414.P3.T1 — GUI check and docs
  - files: `Tests/gui/m414_advanced_modeling.py`, `Engine/Nodes/Geometry/docs/*.md`
  - approach: Bevel a box, reduce an imported scan-like OBJ, remesh it, viewed in the viewer with wireframe. Docs written fresh.
  - verify: the script passes under Xvfb; screenshots sent with SendUserFile for the Feature's UAT.
  - size: M

**Verification gate:** full ctest green including `GeoHalfEdge_Test`, `GeoPolyReduce_Test`, `GeoPolyBevel_Test` and `GeoRemesh_Test`; `Tests/gui/m414_advanced_modeling.py` passes under Xvfb; reduce and remesh of a 1M-face mesh timed in `## Decisions` (no threshold).

## Decisions

- 2026-11-12 — **In-house on `GeoDetail` (user, /cat-plan grill),** not meshoptimizer/CGAL/geogram: attributes and groups survive without conversion.

Shared design: `PLAN/DECISIONS/2026-11-12-geometry-simulation-expansion.md`.
