# M374 - Remove Node Magnify On Ctrl-Scroll

With a node selected (left-clicked) in the node graph, Ctrl+wheel scales that node up or down,
and releasing Ctrl snaps it back to its normal size. The user finds it pointless (user,
2026-11-08): remove it. Ctrl+wheel then zooms the graph exactly as the plain wheel does.

## Scope

**In:**
- Remove the magnify branch in `NodeGraph::wheelEventInternal` and every piece of state that
  exists only for it (`_magnifiedNode`, `_magnifOn`, `_nodeSelectedScaleBeforeMagnif`) and the
  restore-on-release / restore-on-deselect code.

**Out:**
- Ctrl as a modifier elsewhere in the graph (Ctrl shows bend points on press/hides them on
  release; Ctrl-drag moves; Ctrl+Up/Down) — untouched.
- `NodeGui::setScale_natron` itself stays; only the magnify callers go.
- Any change to wheel zoom speed or zoom limits.

**Edge cases:**
- `wheelEventInternal` is also called from the zoom-area drag (`NodeGraph20.cpp`, middle/Alt
  drag zoom) with the Ctrl state; after removal it must zoom the graph there too, never scale
  a node.

## Phase 374.1: Remove the feature

- [x] M374.P1.T1 — Delete ctrl-scroll node magnification from the node graph
  - files: Gui/NodeGraph30.cpp (wheelEventInternal ~172, keyReleaseEvent ~205, deselectNode ~374, selection update ~413), Gui/NodeGraph13.cpp (~101), Gui/NodeGraph10.cpp (~313, `_magnifiedNode = selectedNode`), Gui/NodeGraph.cpp (~285, `_magnifiedNode.reset()`), Gui/NodeGraphPrivate.h (~162-164), Gui/NodeGraphPrivate.cpp (~57-59), Gui/NodeGraph.h and Gui/NodeGraph20.cpp if `wheelEventInternal`'s `ctrlDown` parameter is dropped
  - approach: Remove the `if (ctrlDown && _imp->_magnifiedNode) {...} else` branch so the accumulate-and-scale zoom path always runs. Delete the three `_imp` members, their initialisers, and every block that restores the scale on Ctrl release, on deselect, on selection change and on clear-selection; in keyReleaseEvent keep the bend-point hiding. Drop the now-unused `ctrlDown` parameter from `wheelEventInternal` and update its two callers (NodeGraph30.cpp wheelEvent, NodeGraph20.cpp zoom-area drag); check `grep -rn wheelEventInternal Gui` for any other caller. Afterwards `grep -rn "magnif" Gui` must return nothing.
  - verify: Build compiles; `grep -rn "_magnif\|nodeSelectedScaleBeforeMagnif" Gui` is empty; a GUI script under Xvfb (recipe in build/deeprepro/run-gui.sh, fixtures under build/) creates a node, selects it, sends a Ctrl+wheel QWheelEvent to the NodeGraph viewport, and checks the node's `scale()` is still 1.0 and the view transform's zoom changed — the same zoom as an unmodified wheel event of the same delta.
  - size: M

**Verification gate:** build and ctest green; the Xvfb GUI check above passes (Ctrl+wheel over a selected node zooms the graph and leaves the node's scale at 1.0); bend points still appear while Ctrl is held and hide on release.

## Decisions

- 2026-11-15 — Shared lane with M397 - Label Resize Keeps Node Top-Centre and M374 - Remove Node Magnify On Ctrl-Scroll (user): both ship on `milestone/m397-m374-node-graph-fixes` as one PR, one build, one GUI bench run and one UAT; the PR closes both rows.
- 2026-11-15 — M374.P1.T1 landed (code: c6764af23); build and GUI script verify batched into the shared M397/M374 gate. The script checks node scale through `getSize()` (scene not reachable from Python); confirm it reflects `NodeGui::scale()` when it runs.
- 2026-11-16 — PR #296; Codex review round 1 (4 findings: explicitly sized nodes collapsing to label height — also on main — and three weak test checks) fixed in c04334d29. Release gate green (run 43801587865: m397 43/43, m374 11/11, m55 241/241). UAT AppImage build/appimages/Natron-m397-m374-node-graph-fixes.AppImage with steps .md handed over.
- 2026-11-16 — UAT round 1 (user): Ctrl-click to add a Dot crashes Natron reliably; backdrop labels wrap but should grow arbitrarily wide for long lines (unlike regular nodes). Both are being fixed on the branch. The ugly error/warning text drawing over nodes was requested as a separate milestone (inbox).
- 2026-11-16 — **UAT round 2 passed (user).** Main merged in (3027ec1c0); merging PR #296 once its CI is green. The review round closed earlier (Codex round 1, all findings replied to); the round-2 fixes (Dot bodyRect override, backdrop wrap at the edge) were judged too small for a second round.
