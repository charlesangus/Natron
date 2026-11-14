# M429 - GeoAttribPaint Node

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

A port of Houdini's Attribute Paint SOP for native geometry: paint point (and, where the SOP allows, primitive or vertex) attributes such as `Cd`, masks or any float attribute with brushes in the Viewer's geometry context, outputting the geometry with the painted attribute (user, 2026-11-14, M427 - Native Geometry Texture Painting grill). It is the geometry-output sibling of `GeoTexturePaint`, which outputs UDIM textures. Every control and default of the SOP is ported (read `.local/docs/houdini` first), and it follows `PLAN/DECISIONS/2026-11-08-native-geometry-follows-sops.md`. Brush interaction reuses the paint-host stroke builder from M53 - Projected Texture Painting (M53.P6.T4) where it fits.

Blocked on: F15 - Native Geometry landing (M359 - Geometry Operators' operator conventions, M376 - Geometry Display's geometry context) and M53 - Projected Texture Painting merging into F11 - Texture Painting's branch (its shared brush knobs and stroke builder; this milestone merges into that branch too). Whether point picking from M361 - Geometry Point Editing is needed for its brush hit-testing is decided at elaboration.

Acceptance sketch:
- Painting `Cd` on a `GeoPrimitive` grid in the 3D view sets the attribute on the points under the brush, with the SOP's falloff, opacity and mode controls.
- Painting a new float attribute creates it with the SOP's default; strokes persist, replay and undo.
- The output geometry carries the attribute downstream (`GeoToUSD` shows it as a primvar).
