# M200 - Folded Knob Shared Storage

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Make a folded multi-component knob hold one value or curve that every dimension references, with unfold materialising per-dimension copies, instead of M69's copy-on-edit. Design note first. Moved from M31 - Architectural Cleanup (cancelled 2026-10-25).

Blocked on: further issues surfacing with the current copy-on-edit design. Until then the user is happy with it.

Acceptance sketch:
- A design note the user approves (storage, per-dimension expressions, Python `getValue(dim)`, serialization, undo).
- A keyframe set on a folded knob is visible through every dimension without a copy; unfolding yields independent curves.

Carried from M31.P4.T1 — Make a folded multi-component knob hold one value or curve that its dimensions reference:

  - files: investigation first — `Engine/Knob.h`, `Engine/KnobImpl.h`, `Engine/Knob.cpp` (dimension storage, fold/unfold), `Gui/CurveWidget*.cpp` (M69.P10.T10's copy of dimension 0's curve), project serialization of knob values and curves
  - approach: the user (2026-10-12): "we shouldn't be 'copying' things when the node is in single-value mode, it should be referenced." Today folding only equalizes the current value, and M69.P10.T10 makes curve-editor edits copy dimension 0's curve to every dimension. Investigate making a folded knob hold a single value/curve that every dimension reads, with unfold materialising per-dimension copies. Write up the design (storage, expressions per dimension, Python `getValue(dim)`, serialization, undo) and get the user's go-ahead before implementing; split implementation into further tasks then.
  - verify: a design note the user approves; once implemented, a gtest that a keyframe set on a folded knob is visible through every dimension without a copy, and unfolding yields independent curves.
  - size: L

## Decisions

- 2026-10-25 — **Parked until the copy design shows new problems:** the user's call at M31 triage.
