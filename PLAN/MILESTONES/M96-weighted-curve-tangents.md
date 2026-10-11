# M96 - Weighted Curve Tangents

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Add weights (handle length) to keyframe tangents, so that dragging a curve-editor handle lengthens or shortens it and changes the curve. This touches `KeyFrame` (`Engine/Curve.h:50`), `Engine/Interpolation.cpp`, the curve serialization, the Python API and `CurveWidgetPrivate::refreshKeyTangents`. Today the model is slope only; handle length is computed from the zoom just for drawing.

Proposed Deps: M93 (keyframe archive fields).

Blocked on: M93 - Serialization Boundary (keyframe archive fields), plus a decision on whether this ships before or with M16's format break.

Acceptance sketch:
- A weighted handle changes the curve's shape.
- An unweighted key evaluates exactly as today.
