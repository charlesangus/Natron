# M307 - Remove Viewer Auto-Contrast

Follow-up from the user's 2026-11-02 answer during M234 - Camera Projection: the 3D viewport ignores the viewer's auto-contrast, and the auto-contrast feature itself is to be removed. Delete the viewer's auto-contrast toggle, its knob (M164 - Viewer Node Knobs made it a Viewer knob), the min/max computation and the shader and CPU paths that apply it, its Python API and its documentation. No backward compatibility: saved projects with it on simply lose it.

Deps: M164.


Land after M338 - Viewer Input Keys And Locks, which edits the same files; this is deletions and rebases easily. No Python wrapper exists beyond `getParam("autoContrast")`. Auto-contrast was the only source of a non-zero offset, but the offset plumbing is shared with gain, so it stays and carries 0.

## Scope

**In:** the knob, button, min/max computation, render bypass conditions, enum pixmaps, icon, tests and doc sentence.

**Out:** the offset plumbing (`RenderViewerArgs`, `updateParams->offset`, GPU `deviceProcess.offset`). Gain and gamma behaviour is unchanged.

**Edge cases:** a saved project with `autoContrast` loads without complaint and the key is ignored.

**Assumptions:** the 3D viewport needs no change.

## Phase 307.1: Engine

- [x] M307.P1.T1 — Remove the knob, the min/max code and the render bypasses
  - files: `Engine/ViewerInstance.h`, `Engine/ViewerInstance.cpp`, `Engine/ViewerInstancePrivate.h`, `Tests/ViewerKnobs_Test.cpp`, `Tests/Native/ViewerDeviceRoot_Test.cpp`, `Tests/gui/m164_viewer_knobs.py`
  - approach: remove `kViewerParamAutoContrast`, `ViewerArgs::autoContrast`, `isAutoContrastEnabled`, `onAutoContrastChanged`, `MinMaxVal`, `findAutoContrastVminVmax*`, the vmin/vmax blocks and the cache conditions. Keep the QtConcurrent include, as `QtConcurrent::map` remains. Remove the matching test assertions and `AutoContrastKeepsTheRamPath`.
  - verify: `rg -i autocontrast Engine Tests` is empty apart from ToneEqualizer's unrelated `autoContrastBoost`.
  - size: M

## Phase 307.2: GUI

- [x] M307.P2.T1 — Remove the button, icon, pixmap cases and doc sentence
  - files: `Gui/ViewerTab.h`, `Gui/ViewerTab.cpp`, `Gui/ViewerTab30.cpp`, `Gui/ViewerTabPrivate.h`, `Gui/ViewerTabPrivate.cpp`, `Global/Enums.h`, `Gui/GuiApplicationManager.cpp`, `Gui/GuiResources.qrc`, `docs/cache-invalidation/viewer-and-timeline.md`
  - approach: remove the button, connects, name lists and refresh block, keeping gain and gamma always enabled. Delete `lucide/contrast.svg` with its `.qrc` and `SOURCES.txt` lines. Drop auto-contrast from the doc's bypass list.
  - verify: `rg -i 'auto.?contrast'` outside ToneEqualizer is empty; a viewer control-row shot goes to the user.
  - size: M

**Verification gate:** remote build and debug tests pass; `ViewerKnobs_Test`, `ViewerDeviceRoot_Test` and `m164_viewer_knobs.py` pass; the user has seen the control-row shot.
