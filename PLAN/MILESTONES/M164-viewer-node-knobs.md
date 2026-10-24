# M164 - Viewer Node Knobs

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Per-viewer state becomes knobs on the Viewer node, so it is saved in `project.yaml` like any other node data and is scriptable, linkable and undoable. Today it is `ViewerTab` widget state saved through `ViewerDataSerialization` (`Serialization/ProjectGuiSerialization.h`), and `Engine/ViewerInstance` has no knobs. In scope: gain, gamma, display/view/look, channels, layer and alpha layer, inputs A/B, wipe compositing operator, fps and fps lock, playback frame bounds, user RoI and its enable flag, clip-to-project, full-frame processing, auto-contrast, render scale and proxy. The viewer toolbar widgets become views of those knobs. RB-3 (`origin/master`'s `Engine/ViewerNode*.cpp`, especially `ViewerNodeKnobs.cpp`) is a reference to port from selectively, not wholesale.

Filed 2026-10-23 at the user's request. It follows from M16 - Project Format Redesign's decision to drop `gui.yaml`, which leaves viewer state unsaved until this lands.

Blocked on: M16 - Project Format Redesign (no `gui.yaml`, node schema settled) and M107 - GPU Viewer Hand-off (both touch the viewer render path and `ViewerTab`).

Acceptance sketch:
- Set gain, gamma, display/view and wipe on two viewers, then save, close and reopen; both viewers restore their own values from `project.yaml`, and zoom does not.
- Changing gain from Python or by dragging the toolbar slider moves the same knob, and undo reverts it.
- Changing a viewer knob doesn't invalidate the upstream render cache, only the viewer's own processing.

## Scope

**Out:**
- Transient state, never saved: zoom/pan and `zoomOrPanSinceLastFit`, toolbar visibility (app layout), histograms.
