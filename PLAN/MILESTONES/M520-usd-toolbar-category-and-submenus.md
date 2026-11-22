# M520 - USD Toolbar Category And Submenus

Give USD nodes their own top-level toolbar category, USD, instead of the 3D/USD submenu, with submenus that group them by what they do. All 24 Scene nodes register `.grouping = PLUGIN_GROUP_3D_USD` (`"3D/USD"`). Submenus already work through `/` in the grouping string (`splitGrouping()`, `Engine/Nodes/NativeEffectBase.cpp:220`; `populateNodesMenuGroup`, `Gui/GuiApplicationManager.cpp:1713`). Node colour category comes from `Node::domainCategoryFromGrouping` (`Engine/Node.cpp:6820-6832`), which today matches `3D` plus `USD` and must be updated. A member of F36 - UI Polish Batch II.

## Scope

**In:**
- Top-level USD category with its own icon.
- Submenus (`PLUGIN_GROUP_USD_*` macros), with their nodes:
  - **Prims:** USDCard, USDCube, USDCylinder, USDSphere, USDScopePrim, USDXformPrim
  - **Lights:** USDLight
  - **Cameras:** USDCamera, USDProject3D, USDProjectUV
  - **Materials:** USDAssignMaterial, USDEmissiveMaterial, USDPreviewSurface
  - **Composition:** USDMerge, USDReference, USDVariant, USDPrune
  - **Edit:** USDEditPrim, USDEditTransform, USDSetAttribute
  - **Render:** USDRender, USDRenderSettings
  - **I/O:** USDRead, USDWrite
- An icon per submenu from the vendored lucide set (vendor a missing one from lucide, ISC, noted in `SOURCES.txt`).
- `PLUGIN_GROUP_3D_USD` is removed, so a USD node on another branch fails to compile until it picks a submenu (`DECISIONS/2026-11-22-usd-toolbar-submenus.md`).

**Out:**
- The Geometry category: M522 - Geometry Toolbar Category.
- Grouping changes for any non-USD node: none.
- Old projects: nothing to migrate, grouping is not saved (no backward compat).

**Edge cases:**
- The top-level 3D category is left with no nodes on main; it disappears from the toolbar, which is expected.
- `domainCategoryFromGrouping`: a first segment `USD` maps to `eNodeCategoryUsd3D` at any depth; `3D` with anything else stays `eNodeCategoryNative3D`.
- Tab menu: shows the new paths; search by node name is unaffected.

## Phase 520.1: Category

- [ ] M520.P1.T1 — Move USD nodes to a top-level category with submenus
  - files: `Global/Macros.h`, `Engine/Node.cpp`, `Engine/Nodes/Scene/*.h`/`*.cpp` (the 24 `.grouping` lines), `Gui/GuiApplicationManager.cpp`, `Gui/Resources/Icons/`, `Gui/GuiResources.qrc`, `Gui/Resources/Icons/SOURCES.txt`, `Tests/gui/usd_toolbar_menu.py`
  - approach: replace `PLUGIN_GROUP_3D_USD` with `PLUGIN_GROUP_USD "USD"` and eight `PLUGIN_GROUP_USD_<NAME>` macros (`PLUGIN_GROUP_USD "/Prims"` …); retag the nodes per the In list; grep for every other use of `PLUGIN_GROUP_3D_USD` and the literal `"3D/USD"` (Tab menu, tests, docs, Python) and update it; change `domainCategoryFromGrouping` per Edge cases; add a USD icon to `getIconForGrouping` (a new `natron/usd.svg`, a simple prim-hierarchy glyph in the style of `natron/deep.svg`) and add USD to `namedGroupsOrdered[]` after 3D; give each submenu an icon path.
  - verify: an Xvfb script asserts the toolbar has a USD button, its menu has the eight submenus in order with the listed nodes, every USD node's category colour is unchanged, and no menu item uses the puzzle icon; a screenshot of the open menu goes to the user.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `Tests/gui/usd_toolbar_menu.py` passes under Xvfb at 1920x1080; the menu screenshot is OK'd at F36 - UI Polish Batch II's UAT.
