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

- [x] M455.P1.T1 — Filter node-knob markers by foreground panel and visible page
  - files: `Gui/GuiAppInstance.cpp`, `Gui/GuiAppInstance.h`, `Gui/DockablePanel.cpp`, `Gui/DockablePanel.h`, `Gui/DockablePanelPrivate.cpp`, `Gui/PropertiesBinWrapper.cpp` (or wherever the bin orders panels)
  - approach: give `DockablePanel` a query for its visible page and its foreground state (expanded and top of its bin, or floating), and signals on page change, minimise/expand and bin reorder that call `invalidateNodeKeyframeIndicators()`. In the rebuild, keep only nodes whose panel is foreground and only knobs whose top-level page is the visible page (walk `KnobI::getParentKnob` to the `KnobPage`).
  - verify: syntax-check; P1.T3's script.
  - size: M

- [x] M455.P1.T2 — Roto and Tracker markers follow the same rule
  - files: `Gui/RotoPanel.cpp`, `Gui/TrackerPanel.cpp`, `Gui/GuiAppInstance.cpp`
  - approach: gate Roto's and Tracker's indicator lists on the foreground/visible-page test for the page holding their item list: hide their indicators when it fails, restore them when it passes, driven by the same signals. Don't change how M421 - Stroke Keyframe Indicators builds the Roto list, only whether it is shown.
  - verify: syntax-check; P1.T3's script.
  - size: M

- [x] M455.P1.T3 — GUI check of the visible-page rule
  - files: `Tests/gui/m455_visible_page_markers.py` (new), `Gui/TimeLineGui.cpp` (only if the M305 marker accessor needs extending)
  - approach: a Roto with transform keys and shape keys on different pages, and a Transform node with keys; switch pages, minimise and expand, open both panels and swap which is on top, float one; after each step compare the timeline's markers (M305's read-only accessor) with the expected set; screenshot before and after a page switch.
  - verify: the script passes in the remote gate and fails on main.
  - size: M

**Verification gate:** build and ctest green; the GUI script passes under the remote gate; GUI bench shows no regression; UAT AppImage with steps for the user.

## Decisions

- 2026-11-16 — Both rules apply (user): only an open, foreground panel's visible page contributes markers.

- 2026-11-16 — All three tasks implemented (fb16ad0e2 on milestone/m455-keyframe-markers-for-visible-pages). Panels report foreground/page changes to the main window, which sends one combined refresh; Roto item-knob keys filter by the page of the same-named node knob (changes M421's list, as the Roto example requires); m305 and m421 GUI scripts updated to the new rule. Gate run 43982776533. Open for the user: Tracker user keys hidden with the panel (Out says user keys unchanged); a minimised top panel shows no keys rather than the next expanded panel stepping in.
- 2026-11-16 — **Tracker user keys hide with the panel (user):** they follow the same foreground/visible-page rule as every other key; this overrides the Scope's "user keys unchanged" for the Tracker.
- 2026-11-16 — **A minimised top panel is skipped (user):** the topmost expanded panel of a bin counts as foreground.
- 2026-11-17 — **Closing a floating panel closes it (user):** the window's close button and the panel's own close remove it like a docked panel, rather than the inherited re-dock on top of the bin. Node deletion already does (1145a4494, which also fixed m305's node-delete step).
- 2026-11-17 — Resume note (container restart): gate run 44005758821 on 3e9ae17f2 (m455_visible_page_markers, m305_keyframe_markers, m421_stroke_markers). F29 sibling gates: M422 run 43981874115 (failed jobs re-run after a Wikimedia 429), M452 run 43982171441 (re-run queued after the same 429; if it never re-ran, `gh run rerun 43982171441 --failed`), M454 run 43982263981 (re-run), M456 run 43991079342. PR #297 (no AppStream network fetch) at 4d371e404 awaits CI, then merge and merge main into feature/f29-ui-polish-batch.
- 2026-11-17 — F29 gates: every lane's AppImage step failed on AppStream validation (PR #297's fix) except M422's. Own tests green in M422 (GUI m422_viewer_zoom_snap passes), M452, M455 and M456. M454 had a real failure, fixed in e64a3777c. Single-leg MoonRay timeouts or crashes (M422 USDRenderMoonRayTest, M452 USDRenderMoonRayProjectionTest, M456 RenderDelegates segfault plus SplitOnGapsTest timeout) are treated as load flakes; any repeat after #297 gets a diagnosis. Next: merge #297, merge main into the F29 branch and lanes, re-run every gate.
- 2026-11-17 — Review round 1 (Codex, PR #306) fixed in fc7971cb1: a knob's keys show only when every enclosing tab group shows its tab, and switching a nested tab refreshes the markers (CornerPin To and From sub-tab case added); narration comments removed. Gate 44140295100 green. Merged into feature/f29-ui-polish-batch.
