# M455 - Keyframe Markers For Visible Pages

User request (2026-11-16): keyframes from a background tab must not show on the viewer timeline — e.g. a Roto's transform keys should only show while its Transform page is the visible page. Since M305 - Stale Viewer Keyframe Markers, `GuiAppInstance` derives the node-knob markers from the set of nodes whose panels are open (`Node::getAllKnobsKeyframes`, rebuilt on a coalesced timer), with Roto and Tracker keeping their own `addKeyframeIndicator` calls (`Gui/RotoPanel.cpp`, `Gui/TrackerPanel.cpp`). Neither path looks at which page of the panel (`DockablePanel`'s `QTabWidget`) is showing, or whether the panel is in the foreground.

## Scope

**In:**
- A knob's keys show on the timeline only when its node's panel is open **and** in the foreground **and** the knob sits on that panel's visible page (user chose both rules).
- Foreground: the panel is expanded (not minimised) and is the top panel of its properties bin, or a floating panel window.
- Roto shape keys and Tracker keys follow the same rule for the page their item list lives on.
- Markers update immediately on page switch, panel expand/minimise, and properties-bin reorder.

**Out:**
- Marker styling — none.
- The dope sheet and curve editor, which keep showing everything they show today — none.
- User keys (`timelineUserKeys`) — unchanged.

**Edge cases:**
- Knobs not on any page (hidden, secret): never shown, as today.
- Several foreground panels (several properties bins, floating windows): the union of their visible pages.
- A knob on a page inside a group/sub-tab: shown only when its page is the visible one.
- Next/previous-keyframe navigation reads the same filtered list (as since M305).

## Phase 455.1: Visible-page rule

- [ ] M455.P1.T1 — Filter node-knob markers by foreground panel and visible page
  - files: `Gui/GuiAppInstance.cpp`, `Gui/GuiAppInstance.h`, `Gui/DockablePanel.cpp`, `Gui/DockablePanel.h`, `Gui/DockablePanelPrivate.cpp`, `Gui/PropertiesBinWrapper.cpp` (or wherever the bin orders panels)
  - approach: give `DockablePanel` a query for its visible page and its foreground state (expanded and top of its bin, or floating), and signals on page change, minimise/expand and bin reorder that call `invalidateNodeKeyframeIndicators()`. In the rebuild, keep only nodes whose panel is foreground and only knobs whose top-level page is the visible page (walk `KnobI::getParentKnob` to the `KnobPage`).
  - verify: syntax-check; P1.T3's script.
  - size: M

- [ ] M455.P1.T2 — Roto and Tracker markers follow the same rule
  - files: `Gui/RotoPanel.cpp`, `Gui/TrackerPanel.cpp`, `Gui/GuiAppInstance.cpp`
  - approach: gate Roto's and Tracker's indicator lists on the foreground/visible-page test for the page holding their item list: hide their indicators when it fails, restore them when it passes, driven by the same signals. Don't change how M421 - Stroke Keyframe Indicators builds the Roto list, only whether it is shown.
  - verify: syntax-check; P1.T3's script.
  - size: M

- [ ] M455.P1.T3 — GUI check of the visible-page rule
  - files: `Tests/gui/m455_visible_page_markers.py` (new), `Gui/TimeLineGui.cpp` (only if the M305 marker accessor needs extending)
  - approach: a Roto with transform keys and shape keys on different pages, and a Transform node with keys; switch pages, minimise and expand, open both panels and swap which is on top, float one; after each step compare the timeline's markers (M305's read-only accessor) with the expected set; screenshot before and after a page switch.
  - verify: the script passes in the remote gate and fails on main.
  - size: M

**Verification gate:** build and ctest green; the GUI script passes under the remote gate; GUI bench shows no regression; UAT AppImage with steps for the user.

## Decisions

- 2026-11-16 — Both rules apply (user): only an open, foreground panel's visible page contributes markers.
