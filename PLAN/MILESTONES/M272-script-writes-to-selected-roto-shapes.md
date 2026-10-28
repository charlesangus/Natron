# M272 - Script Writes To Selected Roto Shapes

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Found while fixing M211 - Wavelet Retouch's GUI check (2026-10-28). While a roto shape is selected, its knobs are slaved to the node's knobs of the same name: reading the shape's knob returns the node's value, but writing it only changes the shape's own stored copy (`Engine/KnobImpl.h` ~783, ~898). So `shape.getParam(...).set(...)` from Python on a selected shape is silently lost — e.g. right after `ctx.createEllipse(...)`, which leaves the new shape selected. This is Natron 2's design and affects every shape param in Roto, RotoPaint and Retouch. `Tests/Native/RetouchTestSupport.h` already works around it.

Acceptance sketch:
- Setting a shape param from Python (or any non-GUI path) on a selected shape changes what the shape renders with, and reads back the written value.
- No double-writes or feedback loops between the node's knob and the shape's knob; undo stays one step.
- Options to weigh: forward writes on a slaved shape knob to the master node knob; or stop slaving and have the panel edit the selection directly.
