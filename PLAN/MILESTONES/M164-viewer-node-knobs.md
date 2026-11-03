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

**Prior verification gate:** full build + ctest incl. ViewerKnobs_Test; GUI check passes; GUI bench recorded if Engine/Knob* changes; screenshots shared; AppImage for UAT.

## Open questions (for the user; can start)
- Q1 Also save checkerboard, gain/gamma enable toggles, wipe centre/angle/mix and the current view? Recommendation: yes (per-viewer look state).
- Q2 Give the viewer a settings panel (double-click) so its knobs can be linked/expressed from the UI? Recommendation: yes, the toolbar stays the main view.
- Q3 Animatable? Recommendation: no, but allow expressions and links.

## Decisions

- 2026-11-02 — Audit: P5.T1 landed in dc7f30d6d (a viewer tab stops listening for project loads once it loses its node; the GUI check reopens through File > Open's path). The remote build 41387234910 at that tip passed. Pushed, no uncommitted work, no PR. The branch is 23 commits behind main. What remains is the gate: a new AppImage accepted by the user (UAT round 3).
- 2026-10-28: Phase 2 implemented (build pending). One undo command records only the changed viewer knobs; drags merge (undo ID 25). Widgets follow knob changes with signals blocked and never write knobs; unknown saved layer/alpha/display values show as unlisted entries. Display channels auto-switch to alpha only when a layer is picked. Layer knob stores the entry ID. A timeline range differing from the project's after load/undo/Python counts as edited. Locked fps follows the input's rate, falling back to the project's (old behaviour; the plan said project fps — put to the user).
- 2026-10-28: Phase 1 implemented (build pending). Upstream hashes never include the viewer, but the viewer's own hash keys its cached frames, so every viewer knob is hash-exempt. Display/view/look are text knobs (a missing OCIO name survives; empty = project default) — departs from choice-by-ID. Gain in f-stops (default 0); toolbar 1× / gamma 1 turn the enable toggle off. RB-3 names: clipToFormat, enableRegionOfInterest, enableCheckerBoard, desiredFps; downscale = enableDownscale + downscaleLevel (2–32); wipe angle in radians; wipe centre and RoI canonical. Fps/in/out default from the project at creation. Page "Viewer UI Controls". Phase 2 must stop the toolbar's refresh fallbacks overwriting a saved missing layer/display.
- 2026-10-28: elaborated (consultant); can start before Q1–Q3.

## Phase 164.4: UAT round 1 fixes

- [x] M164.P4.T1 — Make Viewer a normal persistent properties-panel node
  - files: `Engine/ViewerInstance.*`, `Gui/ViewerTab.*`, `Gui/NodeSettingsPanel.*`, viewer serialization tests
  - approach: Let a Viewer open the standard properties panel, persist and restore its node knobs like other nodes, and cover save/reopen with more than one Viewer.
  - verify: A saved project reopens with each Viewer's properties and panel behaviour intact.
  - size: M

- [x] M164.P4.T2 — Frame the Viewer on new and reopened projects
  - files: `Gui/ViewerTab.*`, `Gui/ViewerGL*`, viewer GUI tests
  - approach: Apply the normal fit/framed view once after a fresh script gains a Viewer and after a saved script is opened, without overriding user navigation made after load.
  - verify: The GUI test opens a fresh and a saved project and confirms the image is framed in both.
  - size: M

- [x] M164.P4.T3 — Expose FPS as a Viewer property
  - files: `Engine/ViewerInstance.*`, `Gui/ViewerTab.*`, `Tests/ViewerKnobs_Test.cpp`
  - approach: Keep desired FPS on the Viewer node, show it in the properties panel, and make the player use that knob while preserving the existing locked/input-rate behaviour.
  - verify: Editing FPS in the properties panel updates playback and survives save/reopen.
  - size: M

**Verification gate:** full build and tests pass; the Viewer GUI test covers properties, FPS, framing and save/reopen; a new AppImage is accepted by the user.
- 2026-11-01 — P4 at 7ab6d8e9c and 79c40415c: Viewer has a normal properties panel (Label + its own knobs, no Disable/Info page), closed on creation; viewers fit once a project finishes loading (replacing any saved zoom); viewers are registered headlessly and survive headless load/save; naming a Viewer as a headless render target is refused. To confirm with the user: panel closed on creation, fit replaces saved zoom (implementer).

## Phase 164.5: UAT round 2 fixes

- [x] M164.P5.T1 — Loading a project crashes
  - files: `Gui/ViewerTab.*`, `Engine/ViewerInstance.*`, project load path, `Tests/gui/m164_*.py`
  - approach: User, 2026-11-01: opening any saved project, even a Checkerboard into a Viewer, segfaults on the main thread right after "Loading project:". The crash is in `QReadWriteLock::lockForWrite` on address 0x80, called from a queued slot fired by the File > Open action, so a lock member is reached through a null object. Find the null object (the P4 framing-after-load, Viewer panel or FPS code is the likely suspect) and fix it.
  - verify: The GUI check saves a Checkerboard → Viewer project, then reopens it through the same path File > Open uses (into a fresh window and into the current one), and the app survives and frames the image.
  - size: M

**Verification gate:** full build and tests pass; a new AppImage is accepted by the user.
