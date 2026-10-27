# M261 - Roto Keyframes In Dope Sheet And Curve Editor

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Filed from M163 - Incremental Bezier Edits UAT (user, 2026-10-27), a pre-existing issue: Roto/RotoPaint shape keyframes don't show up in the dope sheet, and show up oddly in the curve editor. Investigate how Bezier shape keys (and per-item knob keys) are exposed to `DopeSheet`/`CurveWidget` after M163's curve changes, then make both editors show them correctly: one row per shape with its keys, selectable and movable, and curve-editor entries that make sense.

Deps: M163 - Incremental Bezier Edits.
