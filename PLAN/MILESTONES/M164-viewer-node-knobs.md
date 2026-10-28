# M164 - Viewer Node Knobs

Per-viewer state becomes knobs on the Viewer node, so it is saved in `project.yaml` like any other node data and is scriptable, linkable and undoable. Today it is `ViewerTab` widget state saved through `ViewerDataSerialization` (`Serialization/ProjectGuiSerialization.h`), and `Engine/ViewerInstance` has no knobs. In scope: gain, gamma, display/view/look, channels, layer and alpha layer, inputs A/B, wipe compositing operator, fps and fps lock, playback frame bounds, user RoI and its enable flag, clip-to-project, full-frame processing, auto-contrast, render scale and proxy. The viewer toolbar widgets become views of those knobs. RB-3 (`origin/master`'s `Engine/ViewerNode*.cpp`, especially `ViewerNodeKnobs.cpp`) is a reference to port from selectively, not wholesale.

Filed 2026-10-23 at the user's request. It follows from M16 - Project Format Redesign's decision to drop `gui.yaml`, which leaves viewer state unsaved until this lands.

## Scope

**In:** persistent, non-animated knobs for every item above; pending Q1 also checkerboard, gain/gamma enable toggles, wipe centre/angle/mix and the current view. Toolbar widgets and the PyGuiApp viewer API read and write those knobs.

**Out:**
- Transient state, never saved: zoom/pan and `zoomOrPanSinceLastFit`, toolbar visibility (app layout), histograms.
- Transient/app-level state: playback mode, increment, turbo, time format, the colour-picker toggle, sync and pause.

**Edge cases:**
- Gain, gamma, display/view/look, checkerboard and wipe only redraw / re-run the viewer process; they never change an upstream hash (FrameKey keeps its explicit gain/gamma, `Engine/FrameKey.h:48`).
- A saved layer, alpha channel or OCIO display/view/look missing on load keeps its saved ID and shows as missing (M16 choice-by-ID); rendering falls back without overwriting the saved value.
- Inputs A/B saved as input indices; a disconnected index shows none.
- With fps lock on, fps follows the project fps.
- A slider drag is one undo entry on M114's global stack.
- Headless runs: the knobs exist, save, load and are scriptable.

**Assumptions:** the viewer node's own hash is not part of any upstream cache key (checked in P1.T1). `ViewerDataSerialization` is already gone; ViewerInstance has no settings panel (`Engine/ViewerInstance.h:261`).

## Phase 164.1: Engine knobs

- [x] M164.P1.T1 — Colour knobs on ViewerInstance
  - files: Engine/ViewerInstance.{h,cpp}, Engine/ViewerInstancePrivate.h, Tests/ViewerKnobs_Test.cpp
  - approach: gain, gamma, their enable toggles, display/view/look, autoContrast, checkerboard — persistent, readable labels, RB-3 script names where they exist; getters read the knobs; knobChanged calls the existing redraw paths; setEvaluateOnChange(false).
  - verify: a knob set is seen by the getter; upstream hashes unchanged; project.yaml round-trip.
  - size: M
- [x] M164.P1.T2 — Channel, input and wipe knobs
  - files: same, Tests/ViewerKnobs_Test.cpp
  - approach: channels, layer, alpha layer (input-bound via declareLayerKnob), inputs A/B, compositing operator, (Q1) wipe centre/angle/mix.
  - verify: as P1.T1 plus the missing-layer case.
  - size: M
- [x] M164.P1.T3 — Playback, RoI and processing knobs
  - files: same, Tests/ViewerKnobs_Test.cpp
  - approach: fps, fps lock, frame range, userRoI and its enable, clipToProject, fullFrame, renderScale (enable and index).
  - verify: round-trip; a render request fires only for the processing knobs.
  - size: M

## Phase 164.2: Toolbar and Python become views of the knobs

- [x] M164.P2.T1 — Bind the colour row
  - files: Gui/ViewerTab.cpp, Gui/ViewerTab10.cpp, Gui/ViewerTabPrivate.h
  - approach: widget edits set knobs through the global undo stack; knob changes update widgets with signals blocked; remove duplicate member state.
  - verify: Xvfb: drag gain, undo, slider and knob both revert.
  - size: M
- [x] M164.P2.T2 — Bind channels, layers, inputs and the wipe
  - files: Gui/ViewerTab20.cpp, Gui/ViewerTab30.cpp, Gui/ViewerGL.cpp, Gui/ViewerTabPrivate.h
  - approach: same pattern; the wipe handle drag writes the wipe knobs.
  - verify: Xvfb: a Python set moves the combos and the wipe handle.
  - size: M
- [x] M164.P2.T3 — Bind the player, RoI and proxy row
  - files: Gui/ViewerTab40.cpp, Gui/ViewerTab.cpp, Gui/ViewerGL.cpp, Gui/ViewerTabPrivate.h
  - approach: fps box and lock, in/out spin boxes, RoI drag, clip-to-project, full frame, render-scale combo drive the knobs.
  - verify: Xvfb with undo on a RoI drag.
  - size: M
- [x] M164.P2.T4 — PyGuiApp viewer API through the knobs
  - files: Gui/PyGuiApp.{h,cpp}
  - approach: setFrameRange, setCompositingOperator, setAInput/setBInput, setChannels, setProxy* (PyGuiApp.h:73-101) set the knobs; getParam reaches every knob.
  - verify: a setter and getParam().getValue() agree.
  - size: M

## Phase 164.3: End-to-end check

- [x] M164.P3.T1 — Two-viewer GUI script
  - files: Tests/gui/m164_viewer_knobs.py
  - approach: set gain, gamma, display/view and wipe on two viewers from the toolbar and Python; save, close, reopen; per-viewer restore, zoom not restored; undo; upstream cache not invalidated (like cache_invalidation_viewer.py).
  - verify: remote GUI job passes; screenshots.
  - size: M

**Verification gate:** full build + ctest incl. ViewerKnobs_Test; GUI check passes; GUI bench recorded if Engine/Knob* changes; screenshots shared; AppImage for UAT.

## Open questions (for the user; can start)
- Q1 Also save checkerboard, gain/gamma enable toggles, wipe centre/angle/mix and the current view? Recommendation: yes (per-viewer look state).
- Q2 Give the viewer a settings panel (double-click) so its knobs can be linked/expressed from the UI? Recommendation: yes, the toolbar stays the main view.
- Q3 Animatable? Recommendation: no, but allow expressions and links.

## Decisions

- 2026-10-28: Phase 2 implemented (build pending). One undo command records only the changed viewer knobs; drags merge (undo ID 25). Widgets follow knob changes with signals blocked and never write knobs; unknown saved layer/alpha/display values show as unlisted entries. Display channels auto-switch to alpha only when a layer is picked. Layer knob stores the entry ID. A timeline range differing from the project's after load/undo/Python counts as edited. Locked fps follows the input's rate, falling back to the project's (old behaviour; the plan said project fps — put to the user).
- 2026-10-28: Phase 1 implemented (build pending). Upstream hashes never include the viewer, but the viewer's own hash keys its cached frames, so every viewer knob is hash-exempt. Display/view/look are text knobs (a missing OCIO name survives; empty = project default) — departs from choice-by-ID. Gain in f-stops (default 0); toolbar 1× / gamma 1 turn the enable toggle off. RB-3 names: clipToFormat, enableRegionOfInterest, enableCheckerBoard, desiredFps; downscale = enableDownscale + downscaleLevel (2–32); wipe angle in radians; wipe centre and RoI canonical. Fps/in/out default from the project at creation. Page "Viewer UI Controls". Phase 2 must stop the toolbar's refresh fallbacks overwriting a saved missing layer/display.
- 2026-10-28: elaborated (consultant); can start before Q1–Q3.
