# M522 - Geometry Toolbar Category

Give native geometry nodes their own top-level toolbar category, Geometry, instead of 3D. They exist only on F15 - Native Geometry's branch (`feature/f15-native-geometry`), where 16 nodes register `.grouping = PLUGIN_GROUP_3D`: GeoCopyToPoints, GeoDelete, GeoDisplace, GeoGroup, GeoImportOBJ, GeoMerge, GeoNormals, GeoPrimitive, GeoPython, GeoScatter, GeoSubdivide, GeoToUSD, GeoTransform, GeoUVProject, GeoWrangle, USDToGeo. A member of F15 - Native Geometry, so it ships with those nodes; the companion USD change is M520 - USD Toolbar Category And Submenus in F36 - UI Polish Batch II.

## Scope

**In:**
- `PLUGIN_GROUP_GEOMETRY "Geometry"` with the `lucide/box.svg` icon (3D's today), every geometry node retagged.
- `Node::domainCategoryFromGrouping` maps a first segment `Geometry` to `eNodeCategoryNative3D`.

**Out:**
- Geometry submenus: none (not requested).
- USD nodes: M520 - USD Toolbar Category And Submenus.

**Edge cases:**
- If M520 has merged into the F15 branch first, `domainCategoryFromGrouping` already has a USD branch; add Geometry beside it. If not, keep `3D/USD` working and let the later merge resolve it.
- Code that identifies geometry nodes by `PLUGIN_GROUP_3D` (kind checks, display, Tab menu): grep and switch to the new macro.

## Phase 522.1: Category

- [ ] M522.P1.T1 — Move geometry nodes to a Geometry category
  - files: `Global/Macros.h`, `Engine/Node.cpp`, the 16 geometry node `.h`/`.cpp` files on the F15 branch, `Gui/GuiApplicationManager.cpp`, `Tests/gui/geometry_toolbar_menu.py`
  - approach: add the macro; retag the 16 nodes; grep for `PLUGIN_GROUP_3D` uses that mean "geometry" and switch them; map `Geometry` in `domainCategoryFromGrouping`; add a `getIconForGrouping` branch using `lucide/box.svg` and add Geometry to `namedGroupsOrdered[]` after 3D.
  - verify: an Xvfb script asserts a Geometry toolbar button listing all 16 nodes with the box icon, and that each node's category colour equals the Native 3D colour preference; a screenshot goes to the user.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `Tests/gui/geometry_toolbar_menu.py` passes under Xvfb at 1920x1080; the screenshot is OK'd at F15 - Native Geometry's UAT.
