# M519 - Raw And Loop Toolbar Icons

Give the Raw and Loop node-toolbar categories icons that say what they are. Neither has an entry in `getIconForGrouping()` (`Gui/GuiApplicationManager.cpp:1243`), so both fall through to `lucide/puzzle.svg`, the default. Raw gets a camera-sensor icon (`lucide/aperture.svg`, already vendored) and Loop a cycle icon (`lucide/repeat.svg`, already vendored). A member of F36 - UI Polish Batch II.

## Scope

**In:**
- Toolbar and nodes-menu icons for Raw and Loop, themed like the other group icons.
- `PLUGIN_GROUP_RAW` moves to `Global/Macros.h` beside the other group macros, and Raw joins `namedGroupsOrdered[]`.

**Out:**
- Leaf node icons for Loop nodes: none.
- Icons for the USD and Geometry categories: M520 - USD Toolbar Category And Submenus and M522 - Geometry Toolbar Category.

**Edge cases:**
- Raw's position in `namedGroupsOrdered[]`: right after Image, beside the other input categories.

## Phase 519.1: Icons

- [ ] M519.P1.T1 — Add Raw and Loop group icons
  - files: `Global/Macros.h`, `Engine/Nodes/Raw/RawNodeSupport.h`, `Gui/GuiApplicationManager.cpp`, `Gui/GuiDefines.h` or wherever `PixmapEnum` lives, `Gui/GuiResources.qrc`, `Gui/Resources/Icons/SOURCES.txt`, `Tests/gui/toolbar_group_icons.py`
  - approach: move `#define PLUGIN_GROUP_RAW "Raw"` into `Global/Macros.h` (delete it from `RawNodeSupport.h`); add `NATRON_PIXMAP_RAW_GROUP` and `NATRON_PIXMAP_LOOP_GROUP` with switch cases at `GuiApplicationManager.cpp:347-398` pointing at `lucide/aperture.svg` and `lucide/repeat.svg`; add both branches to `getIconForGrouping`; list Raw in `namedGroupsOrdered[]` after Image; add qrc entries if missing and note the two icons in `SOURCES.txt`.
  - verify: an Xvfb script finds the Raw and Loop toolbar buttons and asserts neither icon's `cacheKey` equals the default puzzle icon's; a screenshot of the toolbar goes to the user.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `Tests/gui/toolbar_group_icons.py` passes under Xvfb at 1920x1080; the toolbar screenshot is OK'd at F36 - UI Polish Batch II's UAT.
