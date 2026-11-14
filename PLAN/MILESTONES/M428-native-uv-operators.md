# M428 - Native UV Operators

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

SOP-style UV authoring for native geometry, so procedural geometry can be texture painted (M427 - Native Geometry Texture Painting) and textured: `GeoUVProject` (planar, cylindrical, spherical, polar and camera projection, as UV Project SOP), `GeoUVFlatten` (seam-based unwrap with pins and flattening methods, as UV Flatten SOP) and `GeoUVLayout` (island packing into the 0–1 square and across UDIM tiles, with padding, rotation and scale options, as UV Layout SOP) (user, 2026-11-14). Each ports every control and default of its SOP (read `.local/docs/houdini` first) and follows `PLAN/DECISIONS/2026-11-08-native-geometry-follows-sops.md`: writes the `uv` attribute (vertex class by default), keeps every other attribute and group, and respects tombstones.

Blocked on: F15 - Native Geometry landing — M359 - Geometry Operators fixes the operator conventions (attribute classes, group handling, tombstones) these nodes follow. The flatten and layout algorithms (LSCM or ABF++, a packing method) need choosing at elaboration; check licences against the GPLv3-or-later rule.

Acceptance sketch:
- `GeoUVProject` on a grid, cylinder and sphere matches UV Project SOP's layout for each projection type and its controls.
- `GeoUVFlatten` on a cube with seam edges gives non-overlapping islands with low distortion; pinned points stay where they are pinned.
- `GeoUVLayout` packs islands without overlap within padding, into one tile or across UDIM tiles on request.
- Output of each paints correctly in TexturePaint (via `GeoToUSD` until M427 - Native Geometry Texture Painting lands).
