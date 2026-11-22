# M550 - Classic Graph Shift-Drop Creation

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Bring the port graph's node-creation gesture (M351 - Shader Graph Presentation, M351.P3.T3) to the classic node graph and to plain Tab creation (user, 2026-11-22).

Acceptance sketch:
- Shift-dropping a dragged edge end on empty space opens the Tab menu; the created node is wired at the drop point, with its connectable inputs (or its output, by the end being placed) highlighted, the first one used, and scroll or up/down moving the connection to the next or previous input (including mask and side inputs) until confirmed. One undo step.
- Plain Tab creation with a node selected does the same for the new node's input fed by the selection: the inputs are highlighted and scroll or up/down choose which one is connected.
- A plain (unshifted) drop of an edge end on empty space keeps disconnecting, as today.
- Reuses M351.P3.T3's code through `ConnectorLayout`/`RadialLayout`; no classic-only copy.

Deps: M351 - Shader Graph Presentation.
