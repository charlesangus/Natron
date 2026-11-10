# M397 - Label Resize Keeps Node Top-Centre

A node's position is its top-left corner, and `NodeGui::resize` (`Gui/NodeGui.cpp`) keeps that
corner fixed. When a node's label text changes size, the node grows right and down, so its
horizontal centre drifts away from the nodes it was lined up with (input/output alignment and
snap-to-node both work from centres). This milestone keeps the node's **top edge and
horizontal centre** fixed on content-driven resizes, so the node grows downward and equally
left and right.

## Scope

**In:**
- Resizes driven by the node's own content: the label knob, the node name, the channels
  suffix (`_channelsExtraLabel`), the Merge operator icon and the persistent error/warning
  message. In code that is the `setNameItemHtml()` tail and `refreshSize()`.

**Out:**
- Resizes the user makes with the resize handle (`resizeByUser`) keep the top-left corner.
- Python `setSize()` and project-load size restores keep the top-left corner.
- Backdrops keep their top-left corner: they are frames the user sizes, not content-sized
  nodes.

**Edge cases:**
- First layout of a new node (no laid-out size yet): no shift. The node is placed where the
  creation code put it.
- Project load (`getApp()->getProject()->isLoadingProject()` or
  `getApp()->isCreatingNodeTree()`): no shift, so saved positions load exactly.
- Odd width changes: shift by `dw / 2.0` on the double-valued `pos()`, so a label edit
  followed by undo returns the node to the exact same position with no rounding drift.
- The shift goes through the normal position refresh, so edges, link arrows and
  `positionChanged` update as for any move. It is not an undoable move of its own: undoing
  the label change shrinks the node back around the same top-centre.

**Assumptions:**
- Node positions serialize as doubles, so a half-pixel shift saves and loads unchanged.
  The test in P1.T2 checks this.

## Phase 397.1: Top-Centre Anchoring

- [ ] M397.P1.T1 — Keep the top edge and horizontal centre fixed on content-driven resizes
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGui.h`
  - approach: Route the content-driven callers (the `resize(..., adjustToTextSize)` at the
    end of `setNameItemHtml()` and `refreshSize()`) through one helper, e.g.
    `resizeToContent(int w, int h, bool adjustToTextSize)`. It reads the current laid-out
    width (`getSize()` / `_mtSafeWidth`) before the resize, calls `resize()`, then, if the
    width changed, moves the node by `-(newW - oldW) / 2.0` in x, keeping y, via
    `refreshPosition(x, y, true)` (skip the magnet). Skip the shift when there was no prior
    laid-out size, while the project is loading or the node tree is being created, and for
    backdrops (`mustFrameName()`). Leave `resize()` itself, `resizeByUser()`, `setSize()`
    and the constructor/size-restore callers anchored at the top-left. Keep the
    `_nameItem->adjustSize()` ordering in `setNameItemHtml()` as it is.
  - verify: builds; P1.T2's GUI script passes against the change.
  - size: M

- [ ] M397.P1.T2 — GUI script test for top-centre anchoring
  - files: `Tests/gui/m397_label_resize_anchor.py` (new), registered beside the existing
    `Tests/gui/` scripts the way `Tests/gui/m55_node_graph.py` is
  - approach: Under Xvfb (the `build/deeprepro/run-gui.sh` recipe locally, or
    `build.sh --from-run <id> --gui "<script>"` remotely), create a node, place it with
    `setPosition`, record `getPosition()`/`getSize()`. Then: (a) set the label knob to a
    long multi-line string and check the top y and the horizontal centre `x + w / 2` are
    unchanged within 0.5 px while the width and height changed; (b) rename the node to a
    much longer name, same check; (c) clear the label, check it returns to the original
    position within 0.5 px; (d) `setSize()` on a node keeps its top-left; (e) a Backdrop's
    label change keeps its top-left; (f) save the project, reload it, and check every
    node's position matches the pre-save value exactly.
  - verify: the script passes on the P1.T1 build and fails (case a or b) on `main`.
  - size: M

**Verification gate:** build and ctest green; `Tests/gui/m397_label_resize_anchor.py` passes
under Xvfb; the GUI bench (`tools/bench/gui/run.sh --xvfb`, `compare.py` against
`tools/bench/gui/baselines/xvfb.jsonl`) shows no regression, recorded in `## Decisions`;
a release AppImage with a UAT steps `.md` (edit a node's label and name in a lined-up chain,
the node stays in its column and its top doesn't move) is accepted by the user.

## Decisions

- 2026-11-10 — Anchor at top-centre (user): the node keeps its top edge and horizontal
  centre and grows downward, so a taller label pushes only the bottom edge down.
