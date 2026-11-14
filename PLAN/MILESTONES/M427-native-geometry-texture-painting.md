# M427 - Native Geometry Texture Painting

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Native geometry as a first-class texture-painting source, with no `GeoToUSD` hop (user, 2026-11-14: "native geometry support as a first class member, not via conversion"). Following the SOP/LOP split, texture painting becomes two nodes over one shared base: `USDTexturePaint` (M53 - Projected Texture Painting's `TexturePaint`, renamed during M53) and a new `GeoTexturePaint` with a Geometry input. Both output the same multi-layer UDIM texture image. M427 extracts the shared base (sessions, brushes, paint buffer, bake engine, cache, undo, Static/Animated mode, Bake Output) from `USDTexturePaint`, and puts a mesh-source interface under SceneRaster and TextureProjection with two adapters: `UsdGeomMesh` and `GeoDetail` (polygons and `uv` of any class, read directly). `GeoTexturePaint` selects primitives with a SOP-style group field. Its base under the paint is a connected Base image (UDIM), else `Cd`, else empty. While its panel is open, the Viewer previews the painted result on the matched geometry through the viewer-private session layer (M376 - Geometry Display). A final render still goes through `GeoToUSD` → `USDMaterial`. Design: `PLAN/DECISIONS/2026-11-14-native-geometry-texture-painting.md`.

Blocked on: M53 - Projected Texture Painting merging into F11 - Texture Painting's branch (its TexturePaint core, which this splits; this milestone merges into that branch too), F15 - Native Geometry landing (M358 - Geometry Bridges And Display's attribute conventions, M376 - Geometry Display's geometry context and viewer-private session layer), and M428 - Native UV Operators for the UAT's flattened mesh.

Acceptance sketch:
- The same `uv`'d mesh painted through `GeoTexturePaint` and through `GeoToUSD` → `USDTexturePaint` bakes identical UDIM tiles.
- A primitive group restricts painting to its primitives; overlapping UVs across the selection warn as on the scene side.
- A connected Base image is the base; without one, `Cd`; without either, empty.
- With a `GeoTexturePaint` panel open and the Viewer on geometry containing its input, the painted texture shows on the matched primitives and updates after each bake.
- Every `USDTexturePaint` brush type, session, undo, Static/Animated mode and Bake Output behaves the same on `GeoTexturePaint`, including whatever M314 - Paint-Through Inputs and M315 - Direct Texel Painting have added by then.

## Scope

**In:**
- Shared texture-paint base extracted from `USDTexturePaint`; `USDTexturePaint` keeps its behaviour and tests.
- Mesh-source interface in SceneRaster/TextureProjection with `UsdGeomMesh` and `GeoDetail` adapters.
- `GeoTexturePaint` node: Geometry input, optional Base image input, SOP-style group field, UDIM texture image output with the same layer→material-purpose metadata as `USDTexturePaint`.
- Base precedence: Base image, else `Cd` (scene-linear, interpolated by its attribute class), else empty.
- Viewer preview on matched geometry through the viewer-private session layer while a `GeoTexturePaint` panel is open; nothing is authored into the graph.
- Carrying over every feature on `USDTexturePaint` when M427 starts, M314 - Paint-Through Inputs and M315 - Direct Texel Painting included if landed.

**Out:**
- Renaming `TexturePaint` to `USDTexturePaint` (M53 - Projected Texture Painting, via its PM).
- Attribute painting that outputs geometry (M429 - GeoAttribPaint Node).
- Viewing an image node's 3D source in a 2D/3D-switchable Viewer (M430 - Viewer 2D/3D Switch); the paint nodes adopt it there.
- Authoring UVs (M428 - Native UV Operators).
- Deforming geometry: geometry is static, as in M53 - Projected Texture Painting.
- Material binding on native geometry; rendering stays `GeoToUSD` → `USDMaterial`.

**Edge cases:**
- Any non-polygon primitive in the selection (packed Geometry, packed USD, polyline, point): node error, no painting.
- Selected polygons without `uv`: node error giving the count and pointing at GeoUVProject/GeoUVFlatten (M428 - Native UV Operators).
- `uv` with tuple size 3: the first two components are used, as `GeoUsdConvert` does.
- Animated upstream geometry: baked against one pinned frame, the range's first, as M53 - Projected Texture Painting's Static mode does.
- Upstream topology or `uv` change after painting: strokes are view-space with their cameras, so the next bake re-projects them onto the new layout.

**Assumptions:**
- Unconfirmed: M53 - Projected Texture Painting's `MaterialBake::Backend` interface can host a `Cd`/Base-image backend without changes to its callers.
- Unconfirmed: the shared base can be extracted without changing `USDTexturePaint`'s serialization, so its M53 tests pass unchanged.

**Verification gate (planned):** gtests for parity (identical tiles through both nodes), group selection, the non-polygon and missing-`uv` errors, and base precedence; an Xvfb GUI script with screenshots of the session-layer preview; an AppImage UAT painting a `GeoPrimitive` and a `GeoUVFlatten`ed mesh; `USDTexturePaint`'s M53 tests pass unchanged.

## Decisions

- 2026-11-14 — Two nodes, not one masked input: `GeoTexturePaint` and `USDTexturePaint` over a shared base, after the user weighed a Scene|Geometry kind mask against the split. Each node's controls fit its context (group field and `Cd` vs Prim Pattern and bound materials), as the 2026-09-18 SOP/LOP split asks.
- 2026-11-14 — Preview: session-layer preview on the matched geometry in M427; the Viewer 2D/3D switch is a separate stub, M430 - Viewer 2D/3D Switch (user: the two modes are not mutually exclusive).
- 2026-11-14 — Any non-polygon primitive in the selection is an error, not a skip (user).
- 2026-11-14 — Missing `uv` is a node error naming the count (user).
- 2026-11-14 — Geometry is static, as in M53 - Projected Texture Painting (user, after `rest` and per-session frames were considered: a session's strokes are screen-space on the pose at its frame, so `rest` would misplace paint).
- 2026-11-14 — A connected Base image wins over `Cd` (user: a connected input is an explicit choice).
- 2026-11-14 — Features added to `USDTexturePaint` before M427 starts are carried over to the shared base by M427 (user).
- 2026-11-14 — Gate: parity, GUI and AppImage UAT (user).
