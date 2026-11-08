# M297 - Viewer Node Knobs Follow-ups

Minor issues from the user's 2026-11-01 UAT of M164 - Viewer Node Knobs, logged as follow-ups at the user's request.

Deps: M164 - Viewer Node Knobs.

## Scope

**In:**
- Turning on Enable Region Of Interest does not set the RoI position knobs straight away, and editing those knobs does nothing until the viewer's RoI widget has been touched. The knobs should be filled in when the RoI is enabled, and editing them should move the RoI immediately (user, 2026-11-01).
- The Viewer properties panel's display, view and look controls should be the same dropdown as the viewer toolbar's (user, 2026-11-01).

**Out:** anything else on the Viewer panel.

Stub: to be broken down into tasks when claimed.

## Phase 297.1: Fixes

- [x] M297.P1.T1 — RoI knobs fill on enable and edits apply at once
  - files: `Engine/ViewerInstance.{h,cpp}`, `Gui/ViewerGL.cpp`, `Gui/ViewerTab10.cpp`
  - approach: the engine fills unset RoI knobs when the RoI is turned on (the drawn RoI, else the centre half of the project format) in one render; the render reads the RoI knobs.
  - verify: `ViewerKnobs_Test` `EnablingTheRoIFillsUnsetRoIKnobs`; `m164_viewer_knobs.py` python-roi section.
  - size: S
- [x] M297.P1.T2 — Display, view and look panel knobs are the toolbar's dropdowns
  - files: `Engine/ViewerInstance{.h,.cpp,Private.h}`, `Gui/ViewerTab.cpp`, `Gui/ViewerTab30.cpp`
  - approach: dropdowns with "Project default (X)" / "None" first; names the config lacks are kept; toolbar and undo read and write names.
  - verify: `ViewerKnobs_Test` `DisplayTransformKnobsListTheConfig`, `ValuesSurviveSaveAndLoad`; m164 panel section.
  - size: M
- [x] M297.P1.T3 — Tests for both issues
  - files: `Tests/ViewerKnobs_Test.cpp`, `Tests/gui/m164_viewer_knobs.py`
  - verify: remote gate with the m164 GUI script.
  - size: S
