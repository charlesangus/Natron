# M521 - Easier Pipe Grabbing

Make unconnected inputs and outputs easy to grab: fatter pipes, a longer output stub and a wider grab area, all set in preferences. Today the pen width is the hardcoded `EDGE_PEN_WIDTH 2` (`Gui/Edge.cpp:59`, used at `:142` and `:182`); the unconnected output stub ends 10 px below the node (`Edge.cpp:528`); the unconnected input length is the `disconnectedArrowLength` preference (`Engine/Settings.cpp:1507`, default 30); grab radii are 10 and 18 px in `NodeGraph::hasItemNearbyMouse` (`Gui/NodeGraph10.cpp:~598-632`) and 10 px in `Edge::contains` (`Edge.cpp:779`). A member of F36 - UI Polish Batch II.

## Scope

**In:**
- Node Graph preferences: `pipeWidth` ("Pipe Width", default 3), `disconnectedArrowLength` ("Disconnected Arrow Length", relabelled, default 40), `outputStubLength` ("Output Stub Length", default 20).
- Pipe width applies to every edge, connected or not; arrow heads scale with it.
- Grab radii grow with pipe width and never fall below today's values.
- Changing a preference restyles open graphs at once.

**Out:**
- Pipe shape (bezier/straight): M524 - Vertical Node Style.
- Per-node or per-edge widths: none.

**Edge cases:**
- Overlapping hit areas: the nearest candidate still wins (`pickNearestEdgeCandidate`).
- Zoomed-out graphs: radii stay in screen pixels (`/ zoomFactor`), as today.
- Data-kind pens (deep, scene, raw) keep their colours and dashes at the new width.

**Assumptions:**
- Defaults 3 / 40 / 20 are the planner's pick; the user tunes them at UAT.

## Phase 521.1: Pipes

- [x] M521.P1.T1 — Add pipe preferences and apply the width
  - files: `Engine/Settings.{h,cpp}`, `Gui/Edge.{h,cpp}`, `Gui/NodeGraph*.cpp` (restyle on change), `Tests/gui/pipe_grab.py`
  - approach: add `pipeWidth` and `outputStubLength` `KnobInt`s beside `disconnectedArrowLength` on the Nodegraph page, with getters; relabel the existing knob and set the new defaults. Replace `EDGE_PEN_WIDTH` with the preference in both `Edge` constructors and `getDrawnPen()`, scale `ARROW_SIZE_*` by width / 2, use `outputStubLength` at `Edge.cpp:528`. On a preference change, re-init every edge in open graphs (as other Nodegraph knobs refresh).
  - verify: the Xvfb script sets each preference, asserts an edge's pen width and the output stub's end point follow it, and screenshots a node at defaults for the user.
  - size: M

- [x] M521.P1.T2 — Widen grab areas with the pipe
  - files: `Gui/NodeGraph10.cpp`, `Gui/Edge.cpp`, `Tests/gui/pipe_grab.py`
  - approach: make `grabRadius` `max(10, 2.5 * pipeWidth)` and the unconnected-input and output-stub radius `max(18, 5 * pipeWidth)` (screen px, divided by zoom); `Edge::contains` and `shape()` use the same radius so hover and click agree. Include the unconnected output stub as a candidate if it is not already one.
  - verify: the Xvfb script presses the mouse 15 px beside a dangling input and beside an output stub at default width and asserts a drag starts from each; at width 1 a press 25 px away grabs nothing.
  - size: M

## Phase 521.2: Default grab and explorations

- [ ] M521.P2.T1 — Wider grab at the default width
  - files: `Gui/Edge.cpp`, `Engine/Settings.cpp`, `Tests/gui/pipe_grab.py`
  - approach: grab radius `max(10, 4 * pipeWidth)` for connected pipes and `max(18, 8 * pipeWidth)` for dangling inputs and output stubs; `pipeWidth` default 4.
  - verify: pipe_grab.py asserts the default radii (16 / 32 px) and that width 1 still floors at 10 / 18.
  - size: M

- [ ] M521.P2.T2 — Output nub (exploration)
  - files: `Gui/Edge.cpp`, `Gui/NodeGui.cpp`, `Engine/Settings.{h,cpp}`, `Tests/gui/pipe_grab.py`
  - approach: an unconnected output draws as a filled half circle under the node's bottom edge instead of the arrow stub, radius about the current stub length; grabbing it starts a connection drag as the stub did. Screenshots for the user to choose between stub and nub.
  - verify: GUI script grabs the nub and connects it; screenshots at defaults.
  - size: M

- [ ] M521.P2.T3 — Highlight grabbable pipes on hover (exploration)
  - files: `Gui/NodeGraph*.cpp`, `Gui/Edge.cpp`, `Tests/gui/pipe_grab.py`
  - approach: while the mouse is over the graph with no button down, the dangling input, output stub/nub or pipe that a click would pick up (same search as `hasItemNearbyMouse`) draws highlighted; it clears when the pointer leaves the radius.
  - verify: GUI script moves the mouse beside a dangling input and asserts it is highlighted, then away and asserts it is not; screenshot of the hover.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `Tests/gui/pipe_grab.py` passes under Xvfb at 1920x1080; GUI bench rule (`tools/bench/gui/run.sh --xvfb`, compared to `baselines/xvfb.jsonl`) with no regression, recorded in `## Decisions`; screenshots OK'd at F36 - UI Polish Batch II's UAT.
- 2026-11-24 — **Default grab (user):** grab radii `max(10, 4 × width)` / `max(18, 8 × width)` and default Pipe Width 4. Explore an output nub (a half circle about the stub's size) and hover highlighting of whatever a click would pick up; screenshots to the user.
- 2026-11-24 (PM) — m69_cleanup's [expanded-curve] checks (size knob's two component rows; curve widget visible) fail identically on a pre-M521 main AppImage (build/m434-base, run locally in the published image), so they are pre-existing, not M521's; filed to the inbox. M521's gate is judged without them.
- 2026-11-25 (PM) — merged into F36 as #368. Gate6 was green apart from the m69_cleanup [expanded-curve] checks, which fail on main too (see above).
- 2026-11-28 — **F36 UAT round 1 (user):** arrowhead width ~3× pipe width (3 px pipe → 9 px head); the output nub is always shown and clickable, even on a connected output, to pull another pipe; defaults Pipe Width 3, output style Nub, nub size 20 px. USD icon redo is under M520.
- 2026-11-28 — F36 round 1 UAT fixes green (f36-uat2, main merged 718ef68871): pipe_grab, Dot shake, cleanup and the other graph scripts pass. UAT round 2 AppImage + steps + screenshots sent; USD icon choice A–D and the Dot nub size still open with the user.
- 2026-11-28 — **F36 UAT round 2 (user):** arrowheads 5× the pipe width by default; Preferences for arrowhead width and length; a node's output pipes start at its nub; **Dots (user):** no nub — a hover ring just outside the disk pulls a new pipe, the disk moves the Dot. USD icon options A–D resent.
