# M195 - Shake And Cut Disconnect

Two new ways to unplug pipes: shake a dragged node (as in Houdini or Shake) to disconnect it from the graph, and Ctrl-drag a cut line across empty canvas (as in Houdini) to cut every pipe it crosses.

Scouted facts (2026-10-25): no shake detection, cut line or dedicated disconnect command exists. A disconnect is a `ConnectCommand` with a null new source (`Gui/NodeGraphUndoRedo.cpp:413-474`). Left-press on empty canvas starts a selection rectangle (`Gui/NodeGraph10.cpp:433-440`), and Ctrl is not otherwise used there; Ctrl+Alt+Left zooms (`:241-246`) so the cut uses Ctrl alone. Event states live in `NodeGraphPrivate.h`.

## Scope

**In:**
- Shake: a dragged node that makes at least three fast direction reversals on one axis within about 500 ms disconnects all its inputs and its output, in one undo step, while the drag continues. The pending removal is shown as the pipes disappearing at once.
- Cut: Ctrl+left-drag from empty canvas draws a cut line; pipes crossing it are highlighted live in the Selection colour; releasing removes them in one undo step; Escape cancels.
- Multi-selection shake disconnects the selection from nodes outside it; pipes within the selection stay.

**Out:**
- Kissing and its modifier. (M194 - Kiss Connections)
- Pick-up fixes. (M192 - Reliable Pipe Pick-Up)
- Preferences for shake sensitivity. (none)
- Cutting links other than pipes (expression links, backdrops). (none)

**Edge cases:**
- A shake needs real displacement; sub-threshold jitter and slow back-and-forth do not trigger.
- After a shake fires, the detector resets and needs a fresh shake to fire again.
- Dots and backdrops: shaking a Dot disconnects it like any node; backdrops have no pipes.
- A cut line that crosses no pipe does nothing; Ctrl-drag that begins on a node or pipe is not a cut.
- Releasing Ctrl mid-cut keeps the cut going until the mouse release.
- Shake while Ctrl is held for kissing still fires; kiss preview is dropped.

**Assumptions:**
- Unconfirmed: reversal thresholds (~40 scene px amplitude per swing) feel right; tuned during sign-off.

## Decisions

- 2026-10-25 — shake-effect: disconnects all the node's pipes (inputs and output), detected by three fast reversals in about 500 ms.
- 2026-10-25 — cut-gesture: Ctrl+drag on empty canvas, with live highlight of the pipes to be cut; chosen over Alt-drag (mapped to middle-click) and a held key.

## Phase 195.1: Shake

- [ ] M195.P1.T1 — Pure shake detector
  - files: `Gui/NodeGraphShake.h` (new), `Gui/NodeGraphShake.cpp` (new), `Gui/CMakeLists.txt`, `Tests/NodeGraphShake_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: A small class fed `(timestampMs, scenePos)` samples that tracks direction reversals per axis with a minimum amplitude per swing and a time window, returning true once and resetting. No Qt widgets.
  - verify: gtest: a zigzag fires; slow motion, jitter, straight lines and two reversals do not; it re-arms after firing.
  - size: M

- [ ] M195.P1.T2 — Wire shake to disconnect
  - files: `Gui/NodeGraph20.cpp`, `Gui/NodeGraphPrivate.h`, `Gui/NodeGraph15.cpp`, `Gui/NodeGraphUndoRedo.cpp`
  - approach: Feed each node-drag move into the detector in `moveSelectedNodesBy`. On fire, collect the selection's boundary pipes and apply null-source `ConnectCommand`s live, recorded as children of the drag's parent undo command, committed on mouse release like M194 - Kiss Connections' kisses (reuse its recorded-children mechanism; do not duplicate it).
  - verify: GUI script (T2 of Phase 195.3): shake disconnects all pipes; one undo restores them with the move.
  - size: M

## Phase 195.2: Cut

- [ ] M195.P2.T1 — Segment-versus-pipe intersection helper
  - files: `Gui/EdgeGeometry.h` (new), `Gui/EdgeGeometry.cpp` (new), `Gui/CMakeLists.txt`, `Tests/EdgeGeometry_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: Pure function testing a cut polyline against a pipe line segment (the pipes are straight lines between `Edge::line()` endpoints).
  - verify: gtest: crossing, parallel, touching endpoint, collinear, polyline with several segments.
  - size: S

- [ ] M195.P2.T2 — Cut-line event state, drawing and disconnect
  - files: `Gui/NodeGraph10.cpp`, `Gui/NodeGraph20.cpp`, `Gui/NodeGraph15.cpp`, `Gui/NodeGraphPrivate.h`, `Gui/NodeGraph25.cpp`
  - approach: Add an `eEventStateCutting`. Ctrl+left press on empty canvas enters it instead of the selection rectangle. On move, extend the polyline, draw it, and highlight crossing pipes via `Edge::setUseHighlight`. On release push one parent `QUndoCommand` of null-source `ConnectCommand`s. Escape cancels and clears the highlights.
  - verify: GUI script: cut across two pipes removes both in one undo step; Escape and an empty cut change nothing; Ctrl+Alt zoom still works.
  - size: L

## Phase 195.3: GUI coverage

- [ ] M195.P3.T1 — GUI script for shake and cut
  - files: `Tests/gui/m195_shake_cut.py` (new)
  - approach: Use the `Tests/gui/m55_node_graph.py` helpers. Shake sections: zigzag fires and disconnects, slow drag does not, multi-selection keeps internal pipes. Cut sections: crossing pipes cut and highlighted pre-release in the Selection colour, undo, Escape, plain drag still box-selects.
  - verify: script passes under Xvfb on a `package.sh` AppImage.
  - size: M

**Verification gate:** the full build and `ctest` pass, `Tests/gui/m195_shake_cut.py` and `Tests/gui/m55_node_graph.py` pass under Xvfb on a `package.sh` AppImage, and screenshots of the cut highlight are shared with the user before sign-off. The GUI bench rule applies.
