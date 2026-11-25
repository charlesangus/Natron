# M608 - Geometry Group Picker

Stop typing group names by hand. Every geometry group field (each node's `geoGroupKnob`, declared in `Engine/Nodes/NodeSpec.h` ~:135) gets four helpers, matching the reference app:

1. **Groups dropdown.** It lists the input's groups for the current Group Type, with element counts, plus the values of the string attributes `name` and `path` as `@name=…` entries. Click replaces the field; Ctrl-click appends.
2. **Select in Viewer.** This button takes a component pick in the 3D view and writes it back as a group or index pattern when you press Enter.
3. **Group List overlay.** A viewer overlay that lists the viewed detail's groups. Click selects a group's members, Shift adds and Ctrl removes, and groups can be coloured.
4. **Pattern selection.** Shift+P selects components by a typed pattern.

The model is the prim-path picker on `KnobString` (`KnobString::setPrimPathHint`, `Engine/KnobTypes.h` ~:1174; buttons in `Gui/KnobGuiString.cpp` ~:545-600). The pattern syntax is the existing one (`Engine/Geo/GeoOpUtils.h` ~:154-170).

Member of F40 - 3D Viewer Tools.

## Scope

**In:**
- `KnobString::setGeoGroupHint(...)`, set automatically on every `geoGroupKnob`. It records the group-type knob and the input it reads.
- The dropdown, built from the **last cooked** input detail (never a main-thread cook; UI stays responsive). It shows group names with counts for the current type (Guess lists all types, each tagged), and string-attribute values from `name` and `path` for that class.
- Select in Viewer: enters component pick mode for the field's group type. Enter writes the shortest pattern (`geoSelectElements`-compatible ranges), and Esc cancels. ` (backtick) re-runs it for the field the panel last used.
- Group List overlay: a toggle on M602 - 3D Viewer Toolbars' display bar, listing groups of the viewed detail by class. Click/Shift/Ctrl edit the component selection; per-group colour toggles draw through the guide layer. This replaces the Highlight Group text field.
- Shift+P in the 3D view opens a pattern field that selects matching components.
- Applies to GeoTransform, GeoToUSD, GeoUVProject, GeoScatter, GeoDelete, GeoGroup, GeoWrangle, GeoCopyToPoints, GeoDisplace, GeoSubdivide, GeoNormals and any later `geoGroupKnob` user.

**Out:**
- New pattern syntax; the parser is unchanged; no milestone.
- Group fields of non-geometry nodes; no milestone.

**Edge cases:**
- Input not cooked yet: the dropdown shows "Input not cooked yet" and triggers no cook.
- Hundreds of groups or attribute values: the menu caps at 200 entries per section with a "more…" filter field.
- Names needing quotes (spaces): inserted quoted, as the parser expects (`@path="/a b"`).
- Group Type changed after picking: the dropdown refreshes; the field text is unchanged.
- Select in Viewer while another node is viewed: it views the node's input first, then restores.

**Assumptions:**
- Unconfirmed: the last cooked detail of a node's input can be reached from the GUI thread cheaply (a cache lookup), the way the USD prim menus read the last evaluated scene (M376.P2.T3).

## Phase 608.1: Field helpers

- [ ] M608.P1.T1 — Group hint on group knobs and a cached group summary
  - files: `Engine/KnobTypes.{h,cpp}` (`KnobString`), `Engine/Nodes/NodeSpec.h`, the node-spec registration that creates `geoGroupKnob`s, new `Engine/Geo/GeoGroupSummary.{h,cpp}`, `Tests/Geometry/GeoGroupSummary_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: Add a geo-group hint to `KnobString` naming its group-type knob and input index, and set it wherever `geoGroupKnob` is wired. `GeoGroupSummary` holds group names and counts per class, plus up to N distinct `name`/`path` values per class. It is computed with the detail when it is cooked and stored beside the cached detail, so the GUI reads it without cooking.
  - verify: gtest: the summary of a detail with two point groups, one primitive group and a `name` attribute lists them with correct counts; the hint is set on GeoTransform's `group`.
  - size: M
- [ ] M608.P1.T2 — Groups dropdown on the knob GUI
  - files: `Gui/KnobGuiString.{h,cpp}` (~:545-600), `Gui/Resources/Images/`
  - approach: With the geo-group hint set, add a ▾ button that opens a menu from the input's `GeoGroupSummary`, filtered by the group-type knob's value. Sections are Groups and @name/@path. Click replaces the text and Ctrl-click appends a space-separated token, as one undo step. Quote values with spaces. Show "Input not cooked yet" when there is no summary.
  - verify: `Tests/gui/m608_group_picker.py` under Xvfb: on GeoGroup → GeoTransform, the dropdown lists the group and picking it sets `group`; Ctrl-pick appends.
  - size: M
- [ ] M608.P1.T3 — Select in Viewer and the reselect key
  - files: `Gui/KnobGuiString.cpp`, `Gui/SceneSelectionActions.cpp` (~:824-940), `Gui/SceneViewport.cpp`, `Engine/Geo/GeoOpUtils.{h,cpp}` (pattern writer), `Gui/ActionShortcuts.{h,cpp}`
  - approach: A button that enters pick mode for the field's group type. On Enter, write the selection as the shortest index-range pattern (add `geoWritePattern` beside `geoSelectElements`, tested to round-trip). It reuses the existing code that writes component selections into group knobs. Backtick repeats it for the last field used.
  - verify: gtest: `geoWritePattern` of {0,1,2,5,7,8,9} gives `0-2 5 7-9`, and it parses back to the same set; the GUI script picks three points and finds `group` set.
  - size: M

## Phase 608.2: Viewer group tools

- [ ] M608.P2.T1 — Group List overlay with colouring
  - files: new `Gui/GeoGroupList.{h,cpp}`, `Gui/SceneDisplayBar.cpp`, `Gui/GeoDisplay.{h,cpp}`, `Engine/ViewerInstance.{h,cpp}` (replace `highlightGroup`)
  - approach: A display-bar toggle shows a list widget over the viewport's top-left, built from the viewed detail's `GeoGroupSummary`. Click/Shift/Ctrl edit the component selection through the selection actions. A colour toggle per group adds a guide-layer highlight in a distinct colour. Remove the `highlightGroup` knob and its popup field (no backward compat).
  - verify: the GUI script clicks a group in the list and checks the component selection equals its members; colouring two groups authors two highlight guides.
  - size: M
- [ ] M608.P2.T2 — Shift+P pattern selection
  - files: `Gui/SceneViewport.cpp`, `Gui/SceneSelectionActions.cpp`, `Gui/ActionShortcuts.{h,cpp}`
  - approach: Shift+P opens a small line edit over the viewport. Enter selects the components matching the pattern (`geoSelectElements` on the viewed detail, current pick type), and Esc closes it.
  - verify: the GUI script types `@P.y>0` and checks the selection is the upper half of a sphere's points.
  - size: M

**Verification gate:** CI green; release gate green with `--gui` running `Tests/gui/m608_group_picker.py`; `GeoGroupSummary_Test` passes; screenshots of the dropdown, the reselect flow and the Group List are shared one file per screenshot and collected for F40 - 3D Viewer Tools' UAT.

## Decisions
- 2026-11-25 — All four of the reference app's group helpers: dropdown (with @name/@path values), viewer reselect, Group List overlay and Shift+P pattern selection (user, grill).
- 2026-11-25 — The Group List replaces the Highlight Group text field (planner; follows from the above).
