# M55 - Node Graph Polish

Full title: Polish — node graph interaction

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

Scheduling note (2026-10-08, user): runs together with M46. Before elaborating, evaluate whether Labelmaker-style node-graph info belongs inside node-graph polish; elaborate them as one milestone or two accordingly.

Scoping draft (2026-10-10): `PLAN/DESIGN/2026-10-10-m44-m55-m56-scoping.md` holds a draft phase/task breakdown and the user questions to settle before elaborating. Deps: M24 (confirmed by the user).

## Decisions

- 2026-10-10 — **M46 stays a separate milestone (user):** M55's items are unrelated to Labelmaker, whose real overlap is M56's node-text wrap.
- 2026-10-10 — **Splice is always on while dragging; a modifier suppresses it (user).** It also works for multi-node selections and draws an insertion marker.
- 2026-10-10 — **Foreground render is a modal progress dialog with cancel (user).** The UI is blocked but still repaints; "background" stays today's in-process render with the Progress panel.
