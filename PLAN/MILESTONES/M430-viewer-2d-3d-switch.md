# M430 - Viewer 2D/3D Switch

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Let the Viewer show an image node's 3D source in the 3D view, switched by hand, instead of choosing 2D or 3D only from the active input's data kind (`ViewerTab::onActiveInputDataKindChanged`). `USDRender` shows the scene it renders; `USDTexturePaint` and `GeoTexturePaint` show their input textured with their own output, while the 2D view shows the UDIM image (user, 2026-11-14, M427 - Native Geometry Texture Painting grill: "a nice feature for other nodes, like USDRender, too").

Blocked on: how an image node declares its 3D source and how its output binds for display — settle at elaboration with the user; M376 - Geometry Display's geometry context for the `GeoTexturePaint` case, and M427 - Native Geometry Texture Painting for that node's adoption (adopt it in whichever of the two lands second).

Acceptance sketch:
- Viewing a `USDRender` offers a 2D/3D switch; 3D shows its input scene with the render's camera available.
- Viewing a `USDTexturePaint` (and `GeoTexturePaint`, once landed) in 3D shows its input geometry textured with its output.
- Image nodes without a 3D source offer no switch.
