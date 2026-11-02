# M277 - Graph Creation And Insert

Make shortcut-created and drag-inserted nodes follow normal node-creation placement and connection rules, including viewer-pipe exclusion and nodes whose first input is already occupied.

## Scope

**In:**
- `;` and `'` mask/stencil shortcuts create beneath and connect to the selected node like ordinary Tab-menu creation.
- Drag insert ignores Viewer pipes.
- A dragged node with an existing connection is offered its first open compatible input.

**Out:**
- Kiss connections, owned by M194 - Kiss Connections.
- Pipe-pick-up reliability, already delivered by M192 - Reliable Pipe Pick-Up.

## Phase 277.1: Shared creation placement

- [x] M277.P1.T1 — Route mask and stencil shortcuts through normal creation
  - files: node-graph shortcut handlers, shared node-creation helper, GUI tests
  - approach: Reuse the Tab-menu creation context for selection, preferred input, placement and undo instead of creating shortcut nodes at graph origin.
  - verify: With and without a selected node, both shortcuts produce the same position and connection as the equivalent Tab-menu action.
  - size: M

## Phase 277.2: Drag-insert eligibility

- [x] M277.P2.T1 — Exclude Viewer pipes from drag insertion
  - files: node-graph splice planner/candidate collection, splice tests
  - approach: Mark Viewer connections as non-insertable candidates while leaving normal image, deep and scene pipes eligible.
  - verify: Hovering or dropping on a Viewer pipe never shows or performs an insert; ordinary pipes still do.
  - size: M

- [x] M277.P2.T2 — Offer the first open compatible input on insert
  - files: node-graph splice planner, connection preference helper, splice tests
  - approach: When the dragged node already has inputs, choose its first unconnected compatible input rather than rejecting insertion; preserve existing connections and reject type mismatches.
  - verify: A node with input 0 occupied inserts through input 1, keeps input 0, and undoes as one step.
  - size: M

- [x] M277.P2.T3 — Lay out variadic typed inputs like Merge and Switch
  - files: USDMerge, USD transform and RFVSwitch input metadata, node-edge layout tests
  - approach: Show the two active/default inputs in normal positions, hide unused variadic inputs on the left, and reveal the next input for manual connection without changing multi-connect gestures (Merge and Switch already behave this way). RFVSwitch places its visible inputs as if all were shown; fix that too.
  - verify: The three node families show compact edge layouts and still accept bulk/multi-input connections.
  - size: M

## Phase 277.3: GUI regression

- [x] M277.P3.T1 — Cover shortcut creation and drag insert end to end
  - files: new node-graph GUI script and fixtures
  - approach: Exercise both shortcuts, Viewer-pipe exclusion, preconnected-node insertion, undo/redo and zoomed placement under Xvfb.
  - verify: The GUI script passes and screenshots show the expected placement and pipe routing.
  - size: M

**Verification gate:** full build and ctest pass; node-graph GUI and bench rules pass; the user accepts shortcut placement and drag-insert behaviour in a release AppImage.

## Decisions

- 2026-11-01 — Implemented at 5ddfa06c8. Variadic layout is generic in NodeGui for any native node spec with a variadic input run (USDMerge, USDXformPrim, USDScopePrim, RFVSwitch). Python preset Merges now connect to the selection and open their panel like Tab-created nodes (implementer).
- 2026-11-01 — PR #155 review: a connected node is inserted only through free non-mask inputs (a node with only its mask free gets no insert), and a pipe whose source already feeds the dragged node draws no hint (review fix).
