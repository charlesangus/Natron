# M96 - Weighted Curve Tangents

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Add weights (handle length) to keyframe tangents, so that dragging a curve-editor handle lengthens or shortens it and changes the curve. This touches `KeyFrame` (`Engine/Curve.h:50`), `Engine/Interpolation.cpp`, the curve serialization, the Python API and `CurveWidgetPrivate::refreshKeyTangents`. Today the model is slope only; handle length is computed from the zoom just for drawing.

Proposed Deps: M93 (keyframe archive fields).

Blocked on: M93 - Serialization Boundary (keyframe archive fields), plus a decision on whether this ships before or with M16's format break.

Acceptance sketch:
- A weighted handle changes the curve's shape.
- An unweighted key evaluates exactly as today.
- In bezier mode a curve-editor handle has two states. **Unbroken:** the in and out handles stay aligned, and changing one handle's length changes the other's too. **Broken:** each handle's length and angle change independently of the other. (From the M69 - Minor Cleanup UAT, 2026-10-12.)
- A weighted handle keeps its new length on release; it never snaps back.

## Decisions

- 2026-10-13 — Handle length belongs here, not in M69 - Minor Cleanup: the user decided at the M69 UAT (2026-10-12) that M69 drops its visual-only follow-the-cursor handle drawing instead of implementing length.
