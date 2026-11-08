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

- [ ] M374.P1.T1 — Delete ctrl-scroll node magnification from the node graph
  - files: Gui/NodeGraph30.cpp (wheelEventInternal ~172, keyReleaseEvent ~205, deselectNode ~374, selection update ~413), Gui/NodeGraph13.cpp (~101), Gui/NodeGraph10.cpp (~313, `_magnifiedNode = selectedNode`), Gui/NodeGraph.cpp (~285, `_magnifiedNode.reset()`), Gui/NodeGraphPrivate.h (~162-164), Gui/NodeGraphPrivate.cpp (~57-59), Gui/NodeGraph.h and Gui/NodeGraph20.cpp if `wheelEventInternal`'s `ctrlDown` parameter is dropped
  - approach: Remove the `if (ctrlDown && _imp->_magnifiedNode) {...} else` branch so the accumulate-and-scale zoom path always runs. Delete the three `_imp` members, their initialisers, and every block that restores the scale on Ctrl release, on deselect, on selection change and on clear-selection; in keyReleaseEvent keep the bend-point hiding. Drop the now-unused `ctrlDown` parameter from `wheelEventInternal` and update its two callers (NodeGraph30.cpp wheelEvent, NodeGraph20.cpp zoom-area drag); check `grep -rn wheelEventInternal Gui` for any other caller. Afterwards `grep -rn "magnif" Gui` must return nothing.
  - verify: Build compiles; `grep -rn "_magnif\|nodeSelectedScaleBeforeMagnif" Gui` is empty; a GUI script under Xvfb (recipe in build/deeprepro/run-gui.sh, fixtures under build/) creates a node, selects it, sends a Ctrl+wheel QWheelEvent to the NodeGraph viewport, and checks the node's `scale()` is still 1.0 and the view transform's zoom changed — the same zoom as an unmodified wheel event of the same delta.
  - size: M

**Verification gate:** build and ctest green; the Xvfb GUI check above passes (Ctrl+wheel over a selected node zooms the graph and leaves the node's scale at 1.0); bend points still appear while Ctrl is held and hide on release.
