# M305 - Stale Viewer Keyframe Markers

Follow-up from the user's 2026-11-01 UAT approval of M275 - Viewer Widget Editing: the blue keyframe markers on the viewer timeline are sometimes stale. They show for a node whose properties panel is closed, or for a keyframe that no longer exists. The markers should follow exactly the keyframes of the nodes whose panels are open, and update when a key is added, moved or deleted from any editor (viewer, properties, dope sheet, curve editor).

Deps: M275, M261.

Stub: to be broken down into tasks when claimed.

Cause (read, not run): the marker list is an incremental count of times, `GuiAppInstance::timelineKeyframes` (`Gui/GuiAppInstance.cpp:212,1646-1698`). `Node::hideKeyframesFromTimeline` (`Engine/Node.cpp:8241-8251`) removes the keys the node has now, not the ones it added at show time, so an edit that skipped the list leaves a ghost or a missing key. The dope sheet and curve editor never update the list, and `goToPreviousKeyframe`/`goToNextKeyframe` read the same list.

## Scope

**In:** node-knob markers equal the keys of the nodes whose panels are open, refreshed when a key is added, moved or removed from any editor, Python, undo, or a driver change.

**Out:** roto shape and tracker markers and `timelineUserKeys`, which keep their own calls; marker styling; the panel open/close rule.

**Edge cases:** panel closed shows no markers even if keys change. A deleted node's markers go, and undo restores them. Two nodes with a key on one frame show one marker until both are gone. Project close clears them.

**Assumptions:** roto and tracker keys aren't in `Node::getAllKnobsKeyframes`. Nothing needs the list off the main thread.

## Phase 305.1: Derived markers

- [ ] M305.P1.T1 — Derive node-knob markers from the shown nodes
  - files: `Engine/TimeLineKeyFrames.h`, `Engine/Node.cpp`, `Gui/GuiAppInstance.h`, `Gui/GuiAppInstance.cpp`, `Engine/Knob.cpp`
  - approach: add a virtual `invalidateNodeKeyframeIndicators()` to `TimeLineKeyFrames` (default no-op). `GuiAppInstance` keeps a weak set of shown nodes and a derived `nodeKeys` list, rebuilt by a coalesced zero-timer from `Node::getAllKnobsKeyframes`, then emits `keyframeIndicatorsChanged`. Show and hide update the set and invalidate. `getKeyframes` and next/previous read `nodeKeys` plus the extras. `Engine/Knob.cpp` invalidates at each keyframe mutation point and in `updateDriverKeysOnTimeline`, only for knobs where `showsKeysOnTimeline()` is true.
  - verify: syntax-check the files; a hide followed by an edit leaves no stale entry.
  - size: M

- [ ] M305.P1.T2 — Remove the incremental node-knob marker calls
  - files: `Gui/KnobGui10.cpp`, `Gui/KnobGui20.cpp`, `Gui/KnobUndoCommand.cpp`, `Gui/NodeGraphUndoRedo.cpp`, `Engine/Knob.cpp`
  - approach: delete the node-knob add and remove calls. At `NodeGraphUndoRedo.cpp` ~:1474 call `showKeyframesOnTimeline`. Leave `RotoPanel` and `TrackerPanel` alone.
  - verify: `rg KeyframeIndicator Gui Engine` shows only Roto, Tracker, `GuiAppInstance` and `TimeLineKeyFrames.h`.
  - size: M

## Phase 305.2: GUI check

- [ ] M305.P2.T1 — Check markers across the editors
  - files: `Gui/TimeLineGui.h`, `Gui/TimeLineGui.cpp`, `Tests/gui/m305_keyframe_markers.py` (new)
  - approach: a read-only `Q_INVOKABLE` returning the marker times. On two animated nodes, open and close each panel; set and delete keys via the panel, dope sheet, curve editor, Python and undo/redo; delete and undo-delete a node. After each step compare markers with the open-panel nodes' keys. Screenshot the timeline before and after.
  - verify: the script passes under the remote gate and fails on main.
  - size: M

**Verification gate:** remote build passes; `m305_keyframe_markers.py` passes; the user has seen the screenshots in F10 - Viewer Polish's UAT.
