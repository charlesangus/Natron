# M304 - Roto Points Snapping To Origin

Follow-up from the user's 2026-11-01 UAT approval of M261 - Roto Keyframes In Dope Sheet And Curve Editor: roto control points sometimes snap to 0,0. M224 - Fluid Roto And RotoPaint Interaction fixed one cause (edits made during a live render were overwritten by the next copy from the internal curve). Once both have merged, reproduce on main (drawing, dragging, keyframing in the dope sheet and curve editor), find any remaining cause and fix it with a regression test.

Deps: M224, M261.

Stub: to be broken down into tasks when claimed.
