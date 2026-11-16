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

- [x] M397.P1.T1 — Keep the top edge and horizontal centre fixed on content-driven resizes
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

- [x] M397.P1.T2 — GUI script test for top-centre anchoring
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
- 2026-11-15 — Shared lane with M397 - Label Resize Keeps Node Top-Centre and M374 - Remove Node Magnify On Ctrl-Scroll (user): both ship on `milestone/m397-m374-node-graph-fixes` as one PR, one build, one GUI bench run and one UAT; the PR closes both rows.
- 2026-11-15 — P1.T1 (code: ab0a8e0b4) and P1.T2 (code: 9229ee2e8) landed; `resizeToContent` takes a `hadContentLayout` flag so a new node's first name layout never shifts it (it already has a placeholder width). Shared remote gate (fast, --tests, GUI m397/m374/m55) running.
- 2026-11-15 — Gate: case c failed because content-driven resizes only ever grew (pre-existing on main). `resizeToContent` now lays out from the default size for nodes without an explicit size (`Node::hasSize()`) and not backdrops, so a node shrinks back when its label or name shortens; explicitly sized nodes and backdrops keep grow-only. Test reworked (rename grows a narrow node). Code: e6fce2f85; main merged (2ba9f8c7d); re-gate running.
- 2026-11-15 — Gate green on 2ba9f8c7d (run 43787233474: ctest; m397 28/28, m374 9/9, m55 241/241). GUI bench (Xvfb, gate AppImage, 20261116T034919Z) vs baselines/xvfb.jsonl: 0 regressions; paste_1000 at 3000 nodes is still over its 2 s timeout as in the baseline (that over-budget op is the run's exit 1). Release build for the UAT AppImage running.
- 2026-11-16 — PR #296; Codex review round 1 (4 findings: explicitly sized nodes collapsing to label height — also on main — and three weak test checks) fixed in c04334d29. Release gate green (run 43801587865: m397 43/43, m374 11/11, m55 241/241). UAT AppImage build/appimages/Natron-m397-m374-node-graph-fixes.AppImage with steps .md handed over.
- 2026-11-16 — UAT round 1 (user): Ctrl-click to add a Dot crashes Natron reliably; backdrop labels wrap but should grow arbitrarily wide for long lines (unlike regular nodes). Both are being fixed on the branch. The ugly error/warning text drawing over nodes was requested as a separate milestone (inbox).
- 2026-11-16 — **Backdrop labels (user correction):** the user can always set a backdrop's size, and the label never changes it. The label wraps at the backdrop's edge, never earlier. The first fix (149b06d81, never wrap plus a label-driven minimum width) was wrong and is being reworked.
- 2026-11-16 — Round-2 fixes (Ctrl-click Dot crash: DotGui::bodyRect; backdrop label wraps at the edge, never resizes) pass dot-insert 22/22, label-resize 50/50 and ctrl-wheel 11/11 on run 43827450020. The first dot-insert failures were the script's synthetic Ctrl press, which Qt reports without the Ctrl modifier. UAT round-2 AppImage and steps handed over; Needs stays uat.
