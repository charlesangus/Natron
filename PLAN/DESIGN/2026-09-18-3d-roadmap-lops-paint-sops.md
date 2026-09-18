# Design: 3D roadmap after M20 — LOPs completion, projected texture painting, SOPs-style geometry

2026-09-18. Amends Part 2 of
`2026-09-05-deep-and-3d-native-extensions.md` per its scope-gravity rule.
That doc's decisions stand unchanged: USD is the substrate (model A), a
`ScenePayload` layer stack flows along scene edges, composition happens
only at consumers, the viewport is Storm through `UsdImagingGLEngine`, and
`SceneOps` is a thin seam, not an abstraction over USD. This doc answers
three questions the user raised after that doc was written and turns the
answers into milestones M52, M53 and M54, in that order.

## Question 1 — can the USD node surface be produced mechanically?

Mostly yes, and M19/M20 already built the parts that can't be. USD's data
model is uniform (prims, attributes, relationships, layers, composition
arcs, schemas), so under the layer-stack model most nodes collapse to a few
generic ones:

- **Schema-driven edit/create nodes.** Iterate `UsdSchemaRegistry`,
  generate knobs from each attribute's type, default and doc. One generic
  node parameterized by schema type covers every typed schema and every
  applied API schema. Houdini's *Edit Properties* / *Create Primitive*
  LOPs are literally this.
- **Composition nodes.** Reference/Payload, Variant select, Kind/Purpose,
  Activate/Visibility (M20's `Prune3D`). Each is one Sdf call over a prim
  selection.
- **Material assign, collections, Write.** Already M20.
- **Render.** One Hydra node parameterized by delegate — M20's
  `HydraRender`.

What is *not* mechanical is the plumbing, and M19/M20 own it: the scene
data kind and its hashing/cache (M19.P1), the viewport (M19.P2), time
policy (M19.P1.T3), and the render bridge (M20.P3.T1). Three pieces are
still missing and are what M52 adds:

1. **Authored-only semantics.** A generated node with forty knobs must
   write opinions only for knobs the user touched, or every node stomps
   every attribute with defaults. Solaris solves this with a per-parameter
   enable toggle. This needs knob-level support (`AuthoredKnobSet`).
2. **Knob-type mapping.** Tokens with allowed values, matrices, asset
   paths, arrays and time-sampled arrays have no clean knob equivalent.
   M52 ships a mapping table (`SchemaKnobMapper`) and explicitly skips
   array-valued attributes; those are geometry data and belong to M54.
3. **Prim selection.** Use `SdfPathExpression` (USD ≥ 23.11) evaluated
   over the composed input stage, rather than inventing a pattern
   language. One `PrimSelector` knob helper replaces M20's "path or
   wildcard" per-node parsing.

A Solaris-style Python-over-the-stage node is the cheapest way to cover
the long tail but conflicts with M19.P1.T1's Python-OFF USD build. It is
an open question on the board, not part of M52. `SetAttribute3D` (author
one named attribute of a chosen type on a selection) is the no-Python
escape hatch.

Generated schema nodes feel clunky to artists (Katana and Solaris users
both say so). M20's hand-curated `Camera3D`/`Light3D`/`Transform3D`/
`Material3D` stay the primary UI; `EditPrim3D` is the baseline that makes
every other attribute reachable.

## Question 2 — does geometry editing / texturing fit a USD assembly model?

The mismatch is not with USD, which has a full authoring API and stores
meshes as plain point/index arrays. The mismatch is with the *layer-stack
node graph convention*: edits are sparse opinions, while modeling and
painting produce dense bulk data and destructive edit histories. Houdini
split SOPs from LOPs for exactly this reason; the same split is adopted
here without building a modeling package.

### Texture painting fits well, because it is a 2D problem

Mari's architecture is projection painting: paint in screen space, bake to
UV space. Natron already has RotoPaint and a strong 2D pipeline, so M53
adds only the 3D-specific pieces:

- **`SceneRaster`** — a small CPU triangle rasterizer over a composed
  stage (triangulation via Hydra's `HdMeshUtil`, which USD ships). Two
  modes: camera-space z-buffer (depth, prim id, world position per pixel)
  and UV-space bake (world position and normal per texel of a prim's `st`
  primvar). CPU, GL-free, so it runs in CI under Xvfb.
- **`ProjectTexture`** (scene + image → image) — for every texel of the
  target prim's UV space, take the world point from the UV bake, project
  it through the chosen camera, occlusion-test against the camera z-buffer,
  sample the input image. Output is a normal Natron `Image`, so it goes
  through the image cache and every 2D node downstream.
- **Live textures** — Hydra loads textures through the `Ar` resolver and
  `Hio` image plugins. A `natron://<node>/<plane>?v=<hash>` scheme served
  by an in-process `ArResolver` + `HioImage` plugin lets `Material3D` bind
  a *node output* as a texture with no disk round trip. The `?v=<hash>`
  query makes a changed image a new texture path, which is the simplest
  reliable way to make Storm reload. `WriteScene` exports live textures
  to files beside the USD so exported scenes stay portable.
- **Paint workflow** = `HydraRender` (through camera) → `RotoPaint` →
  `ProjectTexture` (same camera) → 2D `Merge` over the existing texture →
  `Material3D` → `Viewport3D`. Every brush feature is the existing 2D
  RotoPaint. Multi-camera layering is several `ProjectTexture`s and a 2D
  merge. A shipped preset group wires this up as one node.

Storm's `UsdPreviewSurface` has no camera-projection mode, so projection
is a *bake* (`ProjectTexture`), never a live shader. That is also what
Nuke's Project3D and Mari do at export time.

### Geometry: what fits and what doesn't

Fits cleanly (M54):
- **Generators** (card, cube, sphere, cylinder, grid, OBJ import) and
  **whole-array deformers** (transform, displace, subdivide, normals, UV
  project). `VtArray` is copy-on-write, so passing a million-point array
  between nodes costs a pointer.
- **EditGeo-style point tweaks**: a sparse `point index → offset` map
  stored as knob data, applied at cook time. Nuke's classic EditGeo.
  Known caveat, same as Nuke: edits break if upstream topology changes;
  the node detects a count mismatch and warns rather than misapplying.
- **Groups and attribute-driven selection**, which is the thing that makes
  SOP work powerful and has no home in the layer-stack model.

Does not fit and is out of scope: topology-changing interactive tools
(extrude, bevel, loop cut, booleans, remesh, retopo). Each is a
destructive operation with an edit history; in a non-destructive graph
that is a node per click or an opaque blob node. "Basic modeling" is
defined as generators + deformers + point tweak + transform gizmos.

## Question 3 — what does a SOP/LOP split look like here?

A **fourth data kind**, `eDataKindGeometry`, and a `GeoDetail` container:

```
GeoDetail (immutable value, COW parts)
├─ prims : vector<GeoPrim>
│    kind      : mesh | points | curves
│    path      : TfToken                  // becomes the prim path at the bridge
│    points, faceVertexCounts, faceVertexIndices : VtArray
│    attrs     : map<(interpolation, name), VtValue>   // UsdGeom primvar model
│    groups    : map<name, bitset>        // point or prim groups
├─ contentHash : U64
└─ getSizeInBytes()                       // aliasing-blind, as DeepImage
```

It is deliberately UsdGeom's attribute set without a stage, built from USD
types (`VtArray`, `TfToken`, `VtValue`, `Gf*`) behind a pimpl so no pxr
header reaches `Engine/` core — the same rule as `ScenePayload`. Because
the container *is* UsdGeom's vocabulary, the bridge is a lookup table:

- `GeoToScene` authors a detail as prims under a scope path into the
  node's layer, at the current frame by default (option: time samples
  across the range per M19's policy — off by default, since vertex data
  is exactly what that policy says never to bake automatically).
- `SceneToGeo` extracts prims matching a `PrimSelector` expression into a
  detail at the current time, optionally flattening xforms.

What the split buys over "geometry nodes are Stage nodes authoring dense
overrides under a scratch scope":

- Geo nodes mutate arrays directly with COW. No layer per node, no
  recomposition; `GeoDelete` is a filtered copy rather than a
  deactivation plus a rewritten mesh.
- Per-frame semantics like `Image`, so no time-sample policy inside the
  geo chain. Time is a question only at the bridge.
- Groups exist.

What it shares with the scene side, at zero extra cost:

- **Viewport.** Viewing a geometry node wraps it through `GeoToScene` into
  a scratch stage (cached in `StageCache` by detail hash) and renders
  through the same `Viewport3D`. Picking returns prim path + point index,
  which maps back to the detail.
- **Cache and scheduling.** `Cache<GeoDetail>` is the third instantiation
  of the templated cache after `Image` and `DeepImage`, with its own
  budget knob; `renderGeometry()` mirrors M18's `renderDeep()` pull path.
  The deep milestone already paid for the dispatch points.
- **Picking and gizmos.** M20.P2.T1's translate gizmo is reused for point
  drags; M54 adds a point-picking mode to `Viewport3D`.

A `GeoWrangle` node exposes detail arrays to Natron's *own* Python via the
buffer protocol (`memoryview`; numpy optional through `np.frombuffer`).
That is Natron Python plus a tiny CPython extension type — it needs no
pxr Python bindings and does not touch the open question above.

## Effort on top of the USD milestones

| Piece | Size | Where |
|---|---|---|
| Authored-only knobs, schema→knob table, path expressions | Medium | M52 |
| Composition nodes | Small | M52 |
| CPU raster substrate, ProjectTexture | Medium | M53 |
| Live-texture resolver + Hydra reload | Medium | M53 |
| GeoDetail container, cache, renderGeometry pull path | Medium | M54.P1 |
| Bridges, scratch-stage viewing | Small | M54.P2 |
| First operator set (≈10 nodes) + wrangle | Medium | M54.P3 |
| Point picking + GeoEdit | Medium | M54.P4 |
| Booleans, remesh, retopo, interactive topology tools | Large | out of scope |

Roughly a third to a half again on top of M19+M20 for the whole of
M52–M54; M54's operator library then grows linearly per node.

## Sequencing and dependencies

- **M52** requires M20's gate (`SceneOps`, `Prim3D`, `Transform3D`,
  `Prune3D`, `Material3D`, `WriteScene`, the M20.P3.T3 integration test it
  extends).
- **M53** requires M52 (`PrimSelector`) and M20 (`HydraRender`,
  `Material3D`, `Viewport3D`).
- **M54** requires M52 (`PrimSelector`) and M20.P2.T1 (gizmo). It does not
  technically need M53; it runs after M53 by user priority.

## Node vocabulary added by this amendment (the new cap)

- **M52**: `EditPrim3D`, `Prim3D` (gains schema type), `Reference3D`,
  `Variant3D`, `SetAttribute3D`.
- **M53**: `ProjectTexture`, `Material3D` (gains image inputs),
  `WriteScene` (gains texture export), preset group `PaintThroughCamera`.
- **M54**: `GeoToScene`, `SceneToGeo`, `GeoPrimitive`, `GeoImportOBJ`,
  `GeoTransform`, `GeoMerge`, `GeoGroup`, `GeoDelete`, `GeoDisplace`,
  `GeoNormals`, `GeoSubdivide`, `GeoUVProject`, `GeoWrangle`, `GeoEdit`.

As before: these are caps, not floors. Further nodes need a named
milestone and an amendment here first.

## Risks

- **Schema knob explosion** — `EditPrim3D` on a large schema builds many
  knobs; build them lazily per page and skip arrays. Watch property-panel
  performance with `UsdGeomMesh` selected.
- **Live-texture lifetime** — the `Ar` registry holds weak node refs and
  must tolerate a node deleted while Storm still references its path;
  resolve to a missing-asset error, never a dangling pointer.
- **Hydra texture reload** — the `?v=<hash>` path trick is simple but
  means Storm re-uploads the whole texture on every stroke. Acceptable
  for v1; `HdStDynamicUvTextureObject` is the upgrade path if paint
  latency is unacceptable.
- **GeoEdit topology drift** — sparse edits keyed by point index. Detect
  and warn; do not attempt to remap.
- **Scope gravity, again** — M54's operator list is the SOP cap.
