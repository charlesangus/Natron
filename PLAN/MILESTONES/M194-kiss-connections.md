# M194 - Kiss Connections

While dragging nodes, holding Ctrl lets the dragged node's input arrow tips and output "kiss" other nodes, as in Flame: the pending pipe is previewed in the highlight colour, and releasing Ctrl connects it. The mouse stays down so the user can kiss repeatedly (input 1 to node A, input 2 to node B) and carry on dragging. On mouse release the node stays where dropped, or with Alt held snaps back to its pickup position with the new pipes in place.

Scouted facts (2026-10-25): node drag runs through `NodeGraph::moveSelectedNodesBy` (`Gui/NodeGraph20.cpp:335-408`), which moves nodes live and calls `checkForSpliceHint` (`:213-330`); the undo command for the move is pushed only on release (`Gui/NodeGraph15.cpp:373-385`, a parent `QUndoCommand` with `MoveMultipleNodesCommand` and `InsertNodeCommand` children). `ConnectCommand` (`Gui/NodeGraphUndoRedo.h:146`) handles connect/disconnect and can be a child of that parent. Input arrows are `NodeGui::getInputsArrows()`; `Edge::line()` mapped through `mapToScene` gives tip positions. Ctrl during node drag currently only means "don't carry a backdrop's contents" (`NodeGraph20.cpp:~352`), and Ctrl at release opens the node panel when nothing moved.

## Scope

**In:**
- Ctrl held during a single- or multi-node drag aims a kiss; releasing Ctrl commits the previewed kiss; the drag continues; any number of kisses per drag.
- Input kiss: the closest unconnected input arrow tip of the dragged node to the other node, within reach, connects that input to the other node's output. Only unconnected inputs are eligible.
- Output kiss: the dragged node's output near another node connects to that node's first free input (`getPreferredInputForConnection`); no free input means no kiss.
- Preview: the pending pipe is drawn in the Selection colour (M193 - Pipe Hint Highlight Colour) while Ctrl is held and a target is in reach; it vanishes when out of reach or Ctrl is released.
- All kisses made in one drag, plus the move, form one undo step.
- Drop position: stays where released by default; Alt held at mouse release returns the dragged nodes to their pickup position, keeping the new pipes.
- Kissing and drop-on-pipe do not fight: while Ctrl is held the splice hint is suppressed; releasing Ctrl restores it.

**Out:**
- Kissing by pipes (arrow-pick-up drags) rather than node drags. (M192 - Reliable Pipe Pick-Up)
- Shake and cut gestures. (M195 - Shake And Cut Disconnect)
- A Preferences option for the modifier or default drop behaviour. (none)
- Overwriting an already-connected input by kissing. (none)

**Edge cases:**
- Multi-selection: kiss uses the arrows/outputs that point outside the selection; internal pipes are never candidates.
- The target is the dragged node itself, a node that would create a cycle, or one rejected by `handleConnectionError`: not a candidate, no preview.
- Two equidistant candidates: the first by input number.
- Escape during the drag cancels the drag; kisses committed earlier in the drag are rolled back with it (single undo step).
- Ctrl pressed after the mouse is already released does nothing.
- Dots, backdrops and viewers follow the same connection validity rules as ordinary connects.

**Assumptions:**
- Unconfirmed: "within reach" is a fixed scene distance scaled for zoom and DPI (about the 18 px unconnected-stub grab radius).
- Unconfirmed: dragged-node arrows are re-laid out every move by `refreshPosition`, so tip positions are current when read in `moveSelectedNodesBy`.

## Decisions

- 2026-10-25 — kiss-modifier: Ctrl (user's suggestion) rather than Shift; Ctrl stays free of other node-drag meanings except the backdrop-carry flag, which keeps working.
- 2026-10-25 — kiss-commit: releasing Ctrl commits each kiss immediately, so the user can aim, commit, and keep dragging.
- 2026-10-25 — kiss-input-choice: only unconnected inputs are eligible; the nearest arrow tip wins.
- 2026-10-25 — kiss-ends: both inputs and the output can kiss.
- 2026-10-25 — kiss-drop-position: node stays where dropped; Alt at release snaps back with the new pipes.

## Phase 194.1: Kiss planning

- [ ] M194.P1.T1 — Pure kiss planner
  - files: `Gui/NodeGraphKiss.h` (new), `Gui/NodeGraphKiss.cpp` (new), `Gui/CMakeLists.txt`, `Tests/NodeGraphKiss_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: Model on `Gui/NodeGraphSplice.h/.cpp`. `planKiss(candidates, reach)` takes plain structs (tip points of unconnected input arrows with input numbers, the output point, target node rects with free-input info) and returns the single best kiss (which end, which input, which target) or none; nearest wins, ties by input number. No Qt widgets.
  - verify: gtest cases: nearest of several inputs, connected inputs skipped, output to first free input, no free input, out of reach, tie-break.
  - size: M

## Phase 194.2: Preview and commit

- [ ] M194.P2.T1 — Gather candidates and show the kiss preview while Ctrl is held
  - files: `Gui/NodeGraph20.cpp`, `Gui/NodeGraphPrivate.h`, `Gui/NodeGraph.cpp`, `Gui/NodeGraphKiss.cpp`
  - approach: In `moveSelectedNodesBy` (and on Ctrl key press/release during a node drag, via the key events), build the planner inputs from the selection's boundary arrows and the nodes under reach, validate each with the same checks `handleConnectionError` uses, and show one preview edge (a reusable Edge like `_hintInputEdge`, Selection colour). Suppress `checkForSpliceHint` while Ctrl is held.
  - verify: manual under Xvfb: preview appears only with Ctrl held and a target in reach; splice hint returns on Ctrl release.
  - size: L

- [ ] M194.P2.T2 — Commit the kiss on Ctrl release and fold into one undo step
  - files: `Gui/NodeGraph15.cpp`, `Gui/NodeGraph25.cpp`, `Gui/NodeGraphPrivate.h`, `Gui/NodeGraphUndoRedo.cpp`
  - approach: On Ctrl key release during a node drag, apply the kiss live (`ConnectCommand::doConnect`) and record a child `ConnectCommand` for the drag's parent undo command. On mouse release, attach the recorded children to the parent already built in `mouseReleaseEvent` (`:373-385`); Escape (`NodeGraph25.cpp:223-225`) undoes the live connections.
  - verify: gtest/GUI: after two kisses and a drop, one undo restores connections and position; Escape restores connections.
  - size: L

- [ ] M194.P2.T3 — Snap back on Alt at release
  - files: `Gui/NodeGraph15.cpp`, `Gui/NodeGraphUndoRedo.cpp`
  - approach: If Alt is down at mouse release and at least one kiss was committed in the drag, move the selection back by `-_deltaSinceMousePress` before pushing `MoveMultipleNodesCommand` (so the net move is zero and the command is omitted), keeping the connect children. Alt with no kiss does nothing special.
  - verify: GUI script: node returns to pickup position with new pipes, one undo step.
  - size: M

## Phase 194.3: GUI coverage

- [ ] M194.P3.T1 — GUI script for kissing
  - files: `Tests/gui/m194_kiss.py` (new)
  - approach: Use the `Tests/gui/m55_node_graph.py` helpers plus Ctrl key events. Sections: input kiss; output kiss; two kisses in one drag (inputs 1 and 2); only-unconnected eligibility; no Ctrl means no kiss; snap-back with Alt; one undo step; Escape; preview pixels use the Selection colour.
  - verify: script passes under Xvfb on a `package.sh` AppImage.
  - size: M

**Verification gate:** the full build and `ctest` pass, `Tests/gui/m194_kiss.py` and `Tests/gui/m55_node_graph.py` pass under Xvfb on a `package.sh` AppImage, and screenshots of a kiss preview are shared with the user before sign-off. The GUI bench rule applies.
