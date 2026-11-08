# M359 - Geometry Operators

Third member of F15 - Native Geometry. The SOP-style operator set on `GeoDetail` plus the
Python wrangle. Each node behaves like the matching SOP (Transform, Merge, Blast, Normal,
Scatter, Copy to Points, Python) unless a task records why not.
Shared design: M54 - Native Geometry Data Model's intro and Scope, `PLAN/DECISIONS/2026-11-08-native-geometry-follows-sops.md`, `PLAN/DECISIONS/2026-10-27-geometry-bridges-follow-solaris.md`.

Member of F15 - Native Geometry: branches off and merges into `feature/f15-native-geometry` (PLAN-FORMAT §9b); the Feature's UAT covers it.

## Scope

**In:**
- `GeoImportOBJ`, `GeoTransform`, `GeoMerge`, `GeoGroup`, `GeoDelete`, `GeoNormals`, `GeoDisplace`, `GeoSubdivide`, `GeoUVProject`, `GeoScatter`, `GeoCopyToPoints`.
- The Python wrangle (flatten on access, scatter back).

**Out:**
- PolyExtrude, Boolean, Fuse, attribute create/promote nodes and dict attributes (M362 - Geometry Operators II).
- The native kernel language and its wrangle node (M360 - Geometry Kernel Language).
- USD/Alembic import (`USDRead` → `USDToGeo`, M358 - Geometry Bridges And Display).

**Edge cases:**
- Heavily tombstoned inputs: operators keep the holes and iterate in number order; the Python wrangle defragments into a dense view on access.
- A Python wrangle writing an array of the wrong length, or raising: node error with the traceback, render thread unharmed.

**Assumptions:**
- Unconfirmed: the Python node is named `GeoPython`, following Houdini (the kernel node takes `GeoWrangle`).

Requires M54 - Native Geometry Data Model. All nodes are `NativeEffectBase` subclasses in `Engine/Nodes/Geometry/`, each registered through its own `.reg.inc` (the pattern in `Engine/Nodes/Scene/USDCard.reg.inc`), under `PLUGIN_GROUP_3D`. Native geometry uses SOP attribute names (`P`, `N`, `uv`, `Cd`, `pscale`, `orient`, `path`, `name`); only `GeoUsdConvert` maps them to USD names.

## Phase 359.1: Operators

- [x] M359.P1.T1 — `GeoImportOBJ`
  - files: `Engine/Nodes/Geometry/GeoImportOBJ.h`/`.cpp`/`.reg.inc`, `Engine/Nodes/Geometry/ObjParser.h`/`.cpp`, `Tests/GeoImportOBJ_Test.cpp`, a tiny committed `.obj`
  - approach: minimal OBJ parser (v/vt/vn/f with n-gons) into one shared point pool. `vt` → vertex `uv`, `vn` → vertex `N`. Groups become primitive groups, and objects set the `name` primitive attribute, as Houdini's File SOP does. USD/Alembic import is `USDRead` → `USDToGeo`, so this node is OBJ only. A missing file sets a node error.
  - verify: unit test — the committed `.obj` loads with matching counts, `uv`, two groups and two `name` values.
  - size: M

- [x] M359.P1.T2 — `GeoTransform`
  - files: `Engine/Nodes/Geometry/GeoTransform.h`/`.cpp`/`.reg.inc`, `Tests/GeoTransform_Test.cpp`
  - approach: TRS + pivot knobs (same layout as `USDEditTransform`), optional group restriction. It transforms `P` and every attribute by its type info, as the Transform SOP does: point and vector attributes by the matrix, normals by the inverse transpose, quaternions by rotation. Only the pages it writes detach. Packed primitives get their transform multiplied instead, since they have no points to move.
  - verify: unit test — transform moves points; `uv` pages are shared with the input's; `N` stays unit and correct under non-uniform scale; a packed primitive's transform is multiplied and its bounds move with it.
  - size: M

- [x] M359.P1.T3 — `GeoMerge`
  - files: `Engine/Nodes/Geometry/GeoMerge.h`/`.cpp`/`.reg.inc`, `Tests/GeoMerge_Test.cpp`
  - approach: multi-input concatenation as the Merge SOP does it. Point, vertex and primitive pools are appended in input order. An attribute missing on one input is created with its default there, and conflicting types promote as Houdini does (int → float, smaller tuple → larger). Groups union by name. `path` and `name` are ordinary attributes and are never renamed.
  - verify: unit test — merging two details keeps every primitive with consistent vertex → point indices; an attribute present on one input only is defaulted on the other's elements; same-named groups union.
  - size: M

- [x] M359.P1.T4 — `GeoGroup`
  - files: `Engine/Nodes/Geometry/GeoGroup.h`/`.cpp`/`.reg.inc`, `Tests/GeoGroup_Test.cpp`
  - approach: creates a named point, primitive, vertex or edge group from one of: a bounding box, a number pattern (`0-10 20 !5`, Houdini group syntax, plus existing group names), or an attribute comparison (`@attr op value`, scalar attributes). The output is the input with the group added, and only group storage detaches.
  - verify: unit test — a bbox group of a grid selects exactly the left-half points; a number pattern selects exactly those primitives; an attribute comparison over a point float attribute selects the expected set; an edge group from a bbox holds exactly the edges inside it.
  - size: M

- [x] M359.P1.T5 — `GeoDelete`
  - files: `Engine/Nodes/Geometry/GeoDelete.h`/`.cpp`/`.reg.inc`, `Tests/GeoDelete_Test.cpp`
  - approach: removes the members of a named group, or its complement, by tombstoning (M54.P1.T5) rather than compacting. Deleting points removes every primitive and vertex using them; deleting primitives optionally deletes their now-unused points, as the Blast SOP does. Attribute data does not move.
  - verify: unit test — deleting a grid's left-half point group leaves the right half valid in number order, and attribute pages not containing deleted elements stay shared; "delete non-selected" gives the complement; a defragment afterwards gives a dense, identical-content detail.
  - size: M

- [x] M359.P1.T6 — `GeoNormals`
  - files: `Engine/Nodes/Geometry/GeoNormals.h`/`.cpp`/`.reg.inc`, `Tests/GeoNormals_Test.cpp`
  - approach: computes area-weighted `N` on points (default) or vertices (with a cusp angle, as the Normal SOP does), or on primitives. It uses the detail's cached reverse topology. Exposes the computation as a static helper that other nodes (`GeoDisplace`, `GeoEdit`) call after moving points.
  - verify: unit test — normals of a cube are unit length and outward; a flat grid's are all +Z; vertex normals with a 60° cusp keep a cube's faces flat.
  - size: M

- [x] M359.P1.T7 — `GeoDisplace`
  - files: `Engine/Nodes/Geometry/GeoDisplace.h`/`.cpp`/`.reg.inc`, `Tests/GeoDisplace_Test.cpp`
  - approach: moves points along `N` by a scalar from a noise knob or an optional **Image** input sampled at `uv` (luminance), with amplitude and offset knobs and optional group restriction. Recomputes normals via `GeoNormals`' helper afterwards, and computes them first if the input has none.
  - verify: unit test — a flat grid displaced by a constant image moves every point by the same amount along +Z and its normals stay +Z.
  - size: M

- [x] M359.P1.T8 — `GeoSubdivide` via OpenSubdiv
  - files: `Engine/Nodes/Geometry/GeoSubdivide.h`/`.cpp`/`.reg.inc`, `Tests/GeoSubdivide_Test.cpp`, CMake (link OpenSubdiv, already found in `cmake/NatronUsd.cmake`)
  - approach: Catmull-Clark or Loop, level knob 1–4. It interpolates point and vertex attributes through the refiner (vertex `uv` as face-varying channels), and primitive attributes and groups carry to child faces. Uses OpenSubdiv's Far refiner directly rather than Hydra.
  - verify: unit test — cube at level 1 has the expected face count and stays closed; `uv` is interpolated; a primitive group survives on the child faces.
  - size: M

- [x] M359.P1.T9 — `GeoUVProject`
  - files: `Engine/Nodes/Geometry/GeoUVProject.h`/`.cpp`/`.reg.inc`, `Tests/GeoUVProject_Test.cpp`
  - approach: writes vertex `uv` from a planar (axis + TRS) or camera projection (inputs: optional Scene for the camera; camera path knob).
  - verify: unit test — planar projection of a grid yields `uv` equal to its normalized XY; camera projection of a camera-facing quad yields 0–1.
  - size: M

- [x] M359.P1.T10 — `GeoScatter`
  - files: `Engine/Nodes/Geometry/GeoScatter.h`/`.cpp`/`.reg.inc`, `Tests/GeoScatter_Test.cpp`
  - approach: the Scatter SOP's core. Scatters a point count (or density × area) over polygon surfaces, area-weighted, with a seed and an optional primitive-group restriction. Optional relaxation iterations. Optionally interpolates chosen source attributes (`N`, `uv`, `Cd`) onto the points and records `sourceprim`/`sourceprimuv`, as Houdini does. Output is unconnected points.
  - verify: unit test — 1000 points on a unit grid all lie on it, the count is exact, the same seed reproduces them, and interpolated `N` is +Z.
  - size: M

- [x] M359.P1.T11 — `GeoCopyToPoints`
  - files: `Engine/Nodes/Geometry/GeoCopyToPoints.h`/`.cpp`/`.reg.inc`, `Tests/GeoCopyToPoints_Test.cpp`
  - approach: the Copy to Points SOP. Inputs are source geometry and target points. Each copy is oriented by Houdini's template-point attributes, in the same precedence: `transform`, `orient`, `N`/`up`, `pscale`/`scale`, `trans`, `pivot`. "Pack and Instance" (default off, as in Houdini) emits one Packed Geometry primitive per point, all sharing the source detail. Otherwise copies are expanded into the pool. Chosen target attributes are copied onto each copy's elements.
  - verify: unit test — copying a cube onto 100 scattered points yields 100 cubes at the point positions, oriented by `N` and scaled by `pscale`; with Pack and Instance on, 100 packed primitives share one detail allocation.
  - size: M

## Phase 359.2: Python wrangle

- [ ] M359.P2.T1 — Python wrangle over the detail
  - files: `Engine/Nodes/Geometry/GeoPython.h`/`.cpp`/`.reg.inc`, `Engine/Nodes/Geometry/GeoPyBuffer.h`/`.cpp`, `Tests/GeoPython_Test.cpp`
  - approach: Houdini's Python SOP model (node name: Scope assumption). A script knob (multi-line `KnobString`) runs in Natron's embedded Python under the GIL during `renderGeometry()`. A `geo` object exposes attributes per class (`geo.points["P"]`, `geo.prims["name"]`, `geo.vertices["uv"]`, `geo.detail[...]`) via the CPython buffer protocol (a small hand-written extension type, no Shiboken typesystem changes, no pxr Python). On first access it builds a dense, defragmented copy in number order (`copyDense`), shaped (N, tupleSize). At script end it scatters written arrays back (`scatterDense`), so only changed pages detach. Scripts never see pages or holes. Creating an attribute by assignment is allowed. numpy is optional (`np.frombuffer(view)` if present). A wrong-length write or a script error becomes a node error with the traceback.
  - verify: unit test — a script doubling every `P` z doubles Z and leaves untouched pages shared; a syntax error and a wrong-length write each report on the node without aborting the render thread.
  - size: L

**Verification gate:** CI green with every Phase 359.1 and 359.2 test; the pre-existing ctest suite is green; every node in this member is pxr-free.

## Decisions

- 2026-11-08 — Split out of M54 - SOPs-Style Geometry (user, /cat-plan); tasks renumbered from M54.P3.*. Scatter and CopyToPoints joined the set (user, M54 grill).
