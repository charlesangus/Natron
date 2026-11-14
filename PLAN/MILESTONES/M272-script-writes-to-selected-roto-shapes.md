# M272 - Script Writes To Selected Roto Shapes

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Found while fixing M211 - Wavelet Retouch's GUI check (2026-10-28). While a roto shape is selected, its knobs are slaved to the node's knobs of the same name: reading the shape's knob returns the node's value, but writing it only changes the shape's own stored copy (`Engine/KnobImpl.h` ~783, ~898). So `shape.getParam(...).set(...)` from Python on a selected shape is silently lost — e.g. right after `ctx.createEllipse(...)`, which leaves the new shape selected. This is Natron 2's design and affects every shape param in Roto, RotoPaint and Retouch. `Tests/Native/RetouchTestSupport.h` already works around it.

Related (found in M205's GUI check, 2026-10-28): `Layer.getChildren()` wraps every child as a plain `ItemBase` (`Engine/PyRoto.cpp:197`), so scripts can't call BezierCurve/Layer methods on them without re-resolving through `getItemByName`; it should return typed Layer/BezierCurve wrappers as `getItemByName` does.

Acceptance sketch:
- Setting a shape param from Python (or any non-GUI path) on a selected shape changes what the shape renders with, and reads back the written value.
- No double-writes or feedback loops between the node's knob and the shape's knob; undo stays one step.
- Options to weigh: forward writes on a slaved shape knob to the master node knob; or stop slaving and have the panel edit the selection directly.

Related (found in M421 - Stroke Keyframe Indicators' review, 2026-11-14): the same slaving loses data on multi-select. Selecting a second item runs `RotoCurveContext::tieItemKnob` → `contextKnob->cloneAndUpdateGui(item2)` (`Engine/RotoCurveContext.cpp` ~:1411), whose `refreshListenersAfterValueChange` (`Engine/KnobImpl.h` ~:3084, `Engine/Knob.cpp` ~:6094-6138) clones the proxy into every slaved item. Deselecting copies the proxy back via `untieItemKnob`, so selecting strokes on frames 5 and 12 together can permanently move one to the other's frame, and other animated item knobs are overwritten the same way. `onMasterKeyFrameSet`/`Removed` (`Engine/Knob.cpp` ~:151-210) do it for key edits. This is the one place to fix the slaving (the user asked that it be fixed in one place only); M421 reads each item's own values and depends on this fix for multi-select markers to be right. Acceptance adds: selecting and deselecting several items never changes any item's values or keys; the panel shows the last-selected item's values, and an edit applies to every selected item (as now), so that remains the only way to change several at once.
- 2026-11-14 — Claimed by bosley-native-nodes (user). Elaborating from the stub.
