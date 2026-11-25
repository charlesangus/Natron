# M613 - Geometry Node Handles Rollout

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Give every remaining native geometry node with a spatial control a viewer handle, using M604 - GeoTransform Viewer Handles' node handle API (user, 2026-11-25). Start with an audit: list each geometry node's knobs that have a natural 3D handle (frames, centres, radii, directions, distances) and decide which part of the gizmo, or which purpose-built handle, binds to each. Then add those handles. Likely candidates are GeoScatter's bounds, GeoCopyToPoints' and GeoDisplace's direction or amount, and the F21 - Geometry Operators nodes as they land.

Blocked on: M604 - GeoTransform Viewer Handles landing the node handle API that this rollout uses.

Acceptance sketch:
- An audit lists every geometry node with a spatial knob, each with its handle or a reason for none.
- Every node the audit marks In shows a handle in the 3D view, and its drags write the bound knobs with undo.
