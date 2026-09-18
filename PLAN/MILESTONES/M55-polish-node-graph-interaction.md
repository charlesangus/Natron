# Milestone 55: Polish — node graph interaction

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

A consolidated milestone for independent, low-risk node-graph interaction
fixes, grouped to ship together:
- Input pipes not always shown: a connected input sometimes fails to draw
  its pipe/edge in the node graph — needs a repro before elaboration.
- Splice a node/group into an existing pipe by dropping it on the
  connection, with a highlight during drag showing the insertion point.
- Roto: dragging a feather handle while multiple points are selected should
  adjust feather for all selected points, not just the dragged one.
- Render dialog rework: prompt for frame range instead of assuming the
  project range, and default renders to foreground (background as an
  explicit dialog option).

Blocked on: the input-pipe item needs a concrete repro before elaboration;
the rest are scoping-ready backlog items (user request 2026-09-18,
consolidated 2026-09-18 — see the reorg decision).

Acceptance sketch:
- Every connected input reliably shows its pipe.
- Dragging a node over a pipe previews the splice; dropping inserts it.
- Multi-selected roto points' feather all adjust together on a single drag.
- Render prompts for frame range and defaults to foreground rendering.
