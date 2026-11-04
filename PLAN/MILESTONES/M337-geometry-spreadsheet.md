# M337 - Geometry Spreadsheet

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

A spreadsheet of per-point, per-vertex, per-primitive and per-detail data, like Houdini's Geometry
Spreadsheet, for both USD prims (points and primvars) and geometry details. Raised by the user
during the scene graph redesign (2026-11-04) as a key part of the native geometry plan. It sits
as a tab in M333 - Prim Inspector's inspector.

Blocked on: M54 - SOPs-Style Geometry, which defines `GeoDetail`, the table's other main data source and the element classes the view must cover.

Acceptance sketch:
- Selecting a mesh prim lists its points and primvars at the current frame, with columns per attribute and a row per element, and stays responsive on a mesh with millions of points.
- Viewing a geometry node shows the same table over its `GeoDetail`.
- Selecting rows highlights those elements in the viewer.

## Scope

**Out:**
- Editing values from the table: decide when elaborating, alongside M54 - SOPs-Style Geometry's GeoEdit.

**Edge cases:**
- A prim inside an unloaded payload has nothing to show; the tab says it's not loaded and doesn't load it.
