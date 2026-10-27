# M227 - USD Primitive Nodes

Basic primitives for comp 3D, one node per type as in Nuke: `USDCard`, `USDSphere`,
`USDCube` and `USDCylinder`. Each authors a `UsdGeomMesh` with `st` and normals, so
textures and projection work on them. Storm's implicit gprims (`UsdGeomSphere` …) get no
`st`, which rules them out. They are pure sources at `/<NodeName>` per M226 - Scene
Hierarchy From Topology, and they author the project's up axis and units. Survey:
`PLAN/DESIGN/2026-10-26-3d-mvp-survey.md`.

## Scope

**In:**
- Four source nodes with no inputs. Each authors one `UsdGeomMesh` with `faceVarying` `st` (0–1), normals, `subdivisionScheme = none`, and the default `doubleSided` off, except on the card, where it is on.
- Size and segment knobs per type and TRS + pivot knobs, animated as time samples.
- USDCard is a quad in the plane facing the project's forward axis (XY for Y-up, XZ for Z-up), sized 1 unit high with an aspect knob (default the project format's aspect), with rows/columns subdivisions.

**Out:**
- An image input on any primitive (Nuke Card's) — none (user: textures always go through `USDMaterial`).
- Native `GeoDetail` primitives (`GeoPrimitive`) — M54 - SOPs-Style Geometry.
- Gizmos on these nodes — M230 - 3D Transform Widgets.
- Grid, torus, cone and other types — none.

**Edge cases:**
- Segment knobs at their minimum (sphere 3×2, cylinder 3×1): a valid closed mesh.
- Cylinder caps: a knob for each end (default on); with both off it is an open tube.
- Sphere UV seam: duplicated seam vertices in `st` via `faceVarying` indexing, so textures don't smear across the seam.
- A Z-up project: geometry axes follow the project's Up Axis (cylinder along the up axis, card facing the forward axis).

## Phase 227.1: Mesh builders

- [x] M227.P1.T1 — `PrimitiveMeshes`: pure mesh builders for card, sphere, cube and cylinder
  - files: `Engine/Nodes/Scene/PrimitiveMeshes.{h,cpp}`, `Tests/Scene/PrimitiveMeshes_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: functions returning points, faceVertexCounts, faceVertexIndices, normals and `faceVarying` st (values + indices) for each type, given segments, size, caps and up axis. No USD stage involved; plain `VtArray`s behind the pimpl like the rest of Scene.
  - verify: gtest — expected point/face counts per type at default and minimum segments; every index in range; cube and capped cylinder are closed (each edge shared by exactly two faces); normals unit-length and outward; st covers [0,1]; the sphere seam has distinct st at u=0 and u=1.
  - size: M

## Phase 227.2: Nodes

- [x] M227.P2.T1 — `USDCard` node
  - files: `Engine/Nodes/Scene/USDCard.{h,cpp,reg.inc}`, `Engine/AppManager.cpp`, CMake source list, `Tests/Scene/USDPrimitives_Test.cpp`
  - approach: source node over `SceneNodeAuthoring` (Prim Path `/<scriptName>`, TRS + pivot, project up axis/units via M226.P3.T1). Knobs: aspect (default the project format aspect), uniform scale, rows/columns subdivisions, double sided (default on). Defines the mesh with `PrimitiveMeshes`; static knobs author defaults, animated knobs time samples.
  - verify: gtest — the composed stage has a Mesh at `/USDCard1` with the expected counts and st; aspect 2 gives a 2×1 card; renaming the node moves the prim.
  - size: M

- [x] M227.P2.T2 — `USDSphere`, `USDCube` and `USDCylinder` nodes
  - files: `Engine/Nodes/Scene/USDSphere.{h,cpp,reg.inc}`, `USDCube.{h,cpp,reg.inc}`, `USDCylinder.{h,cpp,reg.inc}`, `Engine/AppManager.cpp`, CMake source list, `Tests/Scene/USDPrimitives_Test.cpp`
  - approach: the same pattern as `USDCard`, sharing a small primitive-node base factored out of T1 if T1 didn't already. Sphere: radius, rows/columns. Cube: size xyz, subdivisions per axis. Cylinder: radius, height, rows/columns, top/bottom caps.
  - verify: gtest — each node composes a Mesh of its type's counts; knob edits re-author only that node's layer (layer-handle identity on unrelated nodes).
  - size: M

- [x] M227.P2.T3 — Primitives in a rendered scene
  - files: `Tests/Scene/USDRenderStorm_Test.cpp` (or a sibling strict-GL case), a committed reference image, `Tests/gui/scene_primitives.py`
  - approach: strict-GL USDRender of the user's example graph (Card → Xform, Sphere → Xform, Merge, USDCamera, USDLight) against a committed reference, tolerance-compared; a GUI script under Xvfb shows it in the Viewer's 3D view and saves a screenshot.
  - verify: passes under `build/m63-gui/run-gl-tests.sh` with `NATRON_TEST_REQUIRE_GL=1`; moving the sphere makes it fail; the GUI script passes and writes the screenshot.
  - size: M

**Verification gate:** release gate green; strict-GL primitive render test passes; screenshots of all four primitives (shaded and wireframe) in the Viewer's 3D view shared before sign-off.

## Decisions

- 2026-10-27 — **Primitive defaults (user):** USDCube subdivisions default to 2 per axis. USDCard is always a unit mesh; the project format's aspect is applied once, at creation, through the transform's scale X knob, never by moving vertices (no Aspect knob). Primitives stay light: Nuke's orientation, lens-in, deform, display, sweep and mesh-type controls are not ported; kind/purpose/visibility will come from separate nodes later. Confirmed again 2026-10-27: keep the primitives simple.
- 2026-10-26 — **Material input (user):** every primitive node takes an optional Material input and authors the material under its own prim, binding itself to it (M226.P2.T4).

2026-10-27: Golden recorded; GUI screenshots shared; draft PR #133; UAT pending.
