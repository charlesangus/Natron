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

**Verification gate:** release gate green per `.local/build-mode`; `Tests/gui/pipe_grab.py` passes under Xvfb at 1920x1080; GUI bench rule (`tools/bench/gui/run.sh --xvfb`, compared to `baselines/xvfb.jsonl`) with no regression, recorded in `## Decisions`; screenshots OK'd at F36 - UI Polish Batch II's UAT.
