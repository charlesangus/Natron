# M427 - Native Geometry Texture Painting

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Make native geometry a first-class TexturePaint source, without a `GeoToUSD` hop (user, 2026-11-14: "native geometry support as a first class member, not via conversion"). TexturePaint's input accepts the geometry kind as well as the scene kind. SceneRaster, TextureProjection and the bake path read meshes through a mesh-source interface with two adapters: the existing `UsdGeomMesh` path and a `GeoDetail` adapter that reads polygons and `uv` (point, vertex or primitive class) directly. In geometry context, mesh selection is a SOP-style group field (primitive group, `@path`/`@name` pattern) instead of Prim Pattern. The base under the paint is `Cd` when present, else an optional Base image input (UDIM), else empty (user, 2026-11-14). In the Viewer's geometry context (M376 - Geometry Display), the painted result shows on the geometry through a preview binding in the viewer-private session layer. A final render still goes through `GeoToUSD` → `USDMaterial`, which is ordinary look-dev wiring, not a paint requirement. Design: `PLAN/DECISIONS/2026-11-14-native-geometry-texture-painting.md`.

Blocked on: M53 - Projected Texture Painting landing with F11 - Texture Painting (TexturePaint core, SceneRaster and the bake engine this generalises), and F15 - Native Geometry landing (M358 - Geometry Bridges And Display's attribute conventions, M376 - Geometry Display's geometry context and viewer-private session layer). A `/cat-grill` of this stub follows on 2026-11-14.

Acceptance sketch:
- `GeoPrimitive` (with `uv`) → TexturePaint, no `GeoToUSD`: a stroke in the 3D view bakes into UDIM 1001 with occlusion, identical to the same mesh through `GeoToUSD` → TexturePaint.
- A primitive group restricts painting to its primitives; overlapping UVs across the selection warn as on the scene side.
- `Cd` shows as the base under the paint; a connected Base image replaces it.
- With the Viewer on a geometry-kind TexturePaint, the painted texture shows on the geometry and updates after each bake.
- Every M53 brush type, session, undo, Static/Animated mode and Bake Output behaves the same on both input kinds.
