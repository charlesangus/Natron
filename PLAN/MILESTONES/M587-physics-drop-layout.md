# M587 - Physics Drop Layout

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Settle props by simulation: drop the selected prims under gravity onto collision geometry and author their resting transforms, as layout placement. Revived from the survey's "not recommended" list (user, 2026-11-24). Design reference: Solaris Drop and Edit's physics placement (`.local/docs/houdini/nodes/lop/drop.md`, `edit.md`).

Blocked on: the user's approval to proceed (user, 2026-11-24: every stub from the feature-gap survey waits for a go-ahead); a rigid-body solver, from F18 - Geometry Loops And Solver or a dedicated milestone.

Acceptance sketch:
- Dropping a set of props onto a floor and a table leaves each at rest without interpenetration, authored as transforms only.
