# M323 - SplineWarp Map Input

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Give SplineWarp a Map input like Nuke's iTransform-style map: an image whose pixels drive per-pixel warp amount (or mask the warp spatially), so a matte can localise or scale the spline warp without extra roto (user, 2026-11-04, SplineWarp UAT).

Blocked on: the SplineWarp stack (M147–M152, M274) landing with its Round 2 UAT fixes, since the input plugs into the final render path and knob layout; and the reference semantics (check Nuke's SplineWarp/iTransform docs in .local/docs before designing).

Acceptance sketch:
- A Map input on SplineWarp whose channel (selectable) scales the warp per pixel; unconnected means no change.
- Region-independent, CPU and GPU paths agree; tests and a GUI check.
