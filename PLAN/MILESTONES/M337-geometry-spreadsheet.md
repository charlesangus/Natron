# M337 - Geometry Spreadsheet

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

A read-only spreadsheet of per-point, per-vertex, per-primitive and per-detail data, like
Houdini's Geometry Spreadsheet, for both USD prims (points and primvars) and geometry details.
Raised by the user during the scene graph redesign (2026-11-04) as a key part of the native
geometry plan. It sits as a tab in M333 - Prim Inspector's inspector. Member of F15 - Native
Geometry: branches off and merges into `feature/f15-native-geometry`, and the Feature's UAT
covers it.

Blocked on: M54 - Native Geometry Data Model landing its page API, which the table reads
visible rows from, and M361 - Geometry Point Editing landing its component selection, which
the table syncs with. Tasks written before then would guess at both APIs.

Acceptance sketch:
- Selecting a mesh prim lists its points and primvars at the current frame, with columns per attribute and a row per element, and stays responsive on a mesh with millions of points.
- Viewing a geometry node shows the same table over its `GeoDetail`, with tabs for points, vertices, primitives and detail.
- Selecting rows highlights those elements in the viewer, and a viewer component pick selects and scrolls to the matching rows.

## Scope

**In:**
- Tabs for points, vertices, primitives and detail, for both `GeoDetail` and USD prims (points and primvars by interpolation).
- One column per tuple component (`P[x]`, `P[y]`, `P[z]`). Array attributes show as a compact cell that expands on hover. Packed primitives show their intrinsics (transform, bounds, kind, source path).
- A virtualised model that reads only the visible rows straight from the pages, so millions of rows cost nothing to show.
- A group-membership filter, a "show selected only" filter, and sorting by column, with the sort index built on a worker.
- Two-way selection sync with the Viewer: rows → `HdSelection` highlighting (geometry) or prim/point highlighting (USD); a Viewer component pick → rows.

**Out:**
- Editing values from the table (none). Edits go through `GeoEdit`, `GeoWrangle` and the USD nodes, as in Houdini.
- A full intrinsics view for every primitive type (area, vertex count and so on) (none).

**Edge cases:**
- A prim inside an unloaded payload has nothing to show; the tab says it's not loaded and doesn't load it.
- Sorting 10M rows: the index builds on a worker while the unsorted table stays usable, and a newer sort or a changed input cancels it.
- Tombstoned details: rows follow dense element numbers, never storage offsets.

## Decisions

- 2026-11-08 — **Read-only, as in Houdini (user, /cat-plan grill).** This settles the earlier Out item about editing.
- 2026-11-08 — **Houdini's layout, virtualised (user):** class tabs, a column per component, expandable array cells, packed intrinsics, group filter and worker-built sort.
- 2026-11-08 — **Selection syncs both ways (user);** this adds M361 - Geometry Point Editing as a dependency.
- 2026-11-08 — **Joined F15 - Native Geometry (user);** stays a stub until its blockers land (user).
