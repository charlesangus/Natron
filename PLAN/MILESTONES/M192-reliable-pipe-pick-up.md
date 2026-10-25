# M192 - Reliable Pipe Pick-Up

Clicking a connected pipe to pick it up fails unless the cursor is perfectly still: `_hasMovedOnce` (`Gui/NodeGraph20.cpp:424`) is set by any mouse-move between press and release, and `mouseReleaseEvent` (`Gui/NodeGraph15.cpp:285`) treats that as a drag, so `shouldStartPickUp` (`Gui/NodeGraphPickUp.cpp:31-36`) refuses. Fluid move-in, click, move-out gestures therefore lose the pick-up. A click-carry also only ever carries an input pipe's source end, and an output edge cannot be carried at all. This milestone makes a click a click (standard drag-distance tolerance) and makes pick-up end-aware.

## Scope

**In:**
- A press-to-release movement under Qt's drag distance (DPI-scaled) counts as a click, however many move events occurred. Beyond it, it is a drag, as today.
- Pick-up carries the end of the pipe nearest the click. Clicking near the arrowhead frees the arrowhead end (the downstream node's input); clicking away from it frees the source end.
- Output edges (the pipe seen from the source node's output) become pickable by click, carrying the end nearest the click.
- Dropping a carried pipe on a node connects it; on empty space it disconnects (as drag-release does today); Escape cancels and restores the original connection.

**Out:**
- Highlight colour of drop hints. (M193 - Pipe Hint Highlight Colour)
- Kissing, shake and cut gestures. (M194 - Kiss Connections, M195 - Shake And Cut Disconnect)
- Changing the grab radii (`TO_DPIX(10)`/`TO_DPIX(18)`) in `hasItemNearbyMouse`. (none)

**Edge cases:**
- Ctrl held at release no longer forces a drag for pipe clicks; Ctrl-release on a node still opens its panel.
- A press that starts on a node still wins over an edge, as today.
- Nearest-end ties (click exactly at the midpoint) pick the source end.
- A pipe whose other end is unconnected (a stub) carries its loose end as today.

**Assumptions:**
- The existing carried-arrow state (`_pickedUpArrowInput`, `pickUpArrow`, `dragPickedUpArrowToCursor`) can store which end is carried with a small extension.

## Decisions

- 2026-10-25 — click-vs-drag: standard semantics — mouse released = click, mouse held = drag — with the toolkit's small movement tolerance so jitter during a click is still a click: the user asked for the standard distinction.
- 2026-10-25 — end-aware-rule: the end nearest the click is carried: the user chose this over a fixed arrowhead-means-output mapping.
- 2026-10-25 — empty-space-drop: dropping a carried pipe on empty space disconnects, as drag-release does: consistency with the existing behaviour.

## Phase 192.1: Click versus drag

- [ ] M192.P1.T1 — Make pick-up tolerate movement under the drag distance
  - files: `Gui/NodeGraphPickUp.h`, `Gui/NodeGraphPickUp.cpp`, `Gui/NodeGraph15.cpp`, `Gui/NodeGraphPrivate.h`, `Tests/NodeGraphPickUp_Test.cpp`
  - approach: Replace the boolean `movedSincePress` input of `shouldStartPickUp` with the press-to-release distance (scene or view pixels, compared against `QApplication::startDragDistance()` scaled with `TO_DPIX`). Record the press position in `mousePressEvent` and compute the distance in `mouseReleaseEvent` (`NodeGraph15.cpp:285`) instead of `_hasMovedOnce`; drop Ctrl from the `hasMovedOnce` expression for the edge-release path only. Keep the node-drag path's use of `_hasMovedOnce` untouched.
  - verify: `NodeGraphPickUp_Test` cases for jittered click (picks up), click just over the threshold (drag), and the existing still-click and drag cases, all pass.
  - size: M

## Phase 192.2: End-aware pick-up

- [ ] M192.P2.T1 — Pure end-selection helper
  - files: `Gui/NodeGraphPickUp.h`, `Gui/NodeGraphPickUp.cpp`, `Tests/NodeGraphPickUp_Test.cpp`
  - approach: Add `pickedUpEnd(clickPos, lineP1, lineP2)` returning source or destination end by nearest endpoint, ties to source. No Qt widgets; unit-tested with fake coordinates.
  - verify: gtest cases for click near each end, at the midpoint, and on a degenerate zero-length line.
  - size: S

- [ ] M192.P2.T2 — Carry the nearest end and allow output-edge pick-up
  - files: `Gui/NodeGraph15.cpp`, `Gui/NodeGraph10.cpp`, `Gui/NodeGraphPrivate.h`, `Gui/Edge.cpp`
  - approach: Extend the carried-arrow state with the carried end. Use `pickedUpEnd` on press/release to choose it. Carrying the destination end calls `dragDest`-style following (`Edge.cpp` `dragDest`) and on a node drop reconnects the source to that node's preferred input via `getPreferredInputForConnection`; carrying the source end keeps today's path. Remove `!isOutputEdge` from `shouldStartPickUp`. Drop on empty space issues the `ConnectCommand` with a null source/disconnect; Escape restores.
  - verify: manual under Xvfb via T3's script; both ends carried and dropped on a node connect correctly, empty-space drop disconnects, Escape restores, one undo step each.
  - size: L

## Phase 192.3: GUI coverage

- [ ] M192.P3.T1 — GUI script for click-with-jitter and end-aware pick-up
  - files: `Tests/gui/m192_pipe_pickup.py` (new)
  - approach: Modelled on `Tests/gui/m55_node_graph.py` helpers (`send_mouse`, `begin_drag`, `drag_step`). Sections: press/release with 2 px of intermediate moves picks up; press-drag beyond threshold behaves as a drag; click near arrowhead carries the downstream end, click mid-pipe/near source carries the source end; output-edge click carries; Escape and empty-space drop. Verify through `getInput()`.
  - verify: script passes under Xvfb on a `package.sh` AppImage.
  - size: M

**Verification gate:** the full build and `ctest` pass, `Tests/gui/m192_pipe_pickup.py` and `Tests/gui/m55_node_graph.py` pass under Xvfb on a `package.sh` AppImage, and the GUI bench rule in the board's Context applies to the diff.
