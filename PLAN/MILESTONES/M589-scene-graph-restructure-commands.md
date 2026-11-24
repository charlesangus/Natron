# M589 - Scene Graph Restructure Commands

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Scene Graph panel commands that create the nodes from M575 - USD Restructure And Graft: Rename on a prim's context menu, and drag-to-reparent, each creating or updating a USDRestructure node after the viewed node.

Blocked on: the user's approval to proceed (user, 2026-11-24: every stub from the feature-gap survey waits for a go-ahead); M575 - USD Restructure And Graft landing; and possibly M581 - Edit Scope Design, which decides where interactive edits land.

Acceptance sketch:
- Renaming a prim in the panel adds a USDRestructure node and the panel shows the new name.
- Dragging a prim onto another reparents it through the same node, as one undo step.
