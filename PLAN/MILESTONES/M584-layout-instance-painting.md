# M584 - Layout Instance Painting

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Set dressing in the viewer: a brush that paints and erases PointInstancer instances of chosen prototypes onto a surface, with random scale and rotation, authored into the node's own layer. Design reference: Solaris Paint Instances and Edit (`.local/docs/houdini/nodes/lop/paintinstances.md`, `edit.md`).

Blocked on: the user's approval to proceed (user, 2026-11-24: every stub from the feature-gap survey waits for a go-ahead); M576 - Point Instance Extract And Prune for the instance-editing groundwork.

Acceptance sketch:
- Painting on a ground mesh adds instances under the brush, and erasing removes them; each stroke is one undo step.
- The result is a valid UsdGeomPointInstancer that renders in every bundled delegate.
