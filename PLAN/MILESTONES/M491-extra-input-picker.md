# M491 - Extra Input Picker

Make every input of a node reachable without moving other arrows. Today `NodeGui::initializeInputs` (`Gui/NodeGui.cpp`) fans regular inputs over the top arc and puts the first two masks at angles 0 and π, so a later input stacks on another arrow. RotoPaint's Vectors input is one, and the user has to drag other inputs aside to reach it (user, F8 - Motion Vectors UAT, 2026-11-17). Dropping a dragged output edge on a node body connects the node's preferred input (`NodeGraph::mouseReleaseEvent`, `Gui/NodeGraph15.cpp:296-316`, via `getPreferredInputForConnection`). This milestone adds an input picker: a plain drop keeps today's behaviour, Ctrl+drop on the body opens a list of the node's inputs, and the node's right-click menu gets "Connect Input ▸ <name>". Drawing is unchanged.

## Scope

**In:**
- An input picker popup listing every input by label, with connected inputs marked by their source's name and mask inputs marked. Picking one connects it as one undo step.
- Ctrl+drop of an output edge on a node body opens the picker. A plain drop behaves exactly as today.
- Right-click on a node, with exactly one other node selected, offers "Connect Input ▸ <label>". It connects the selected node to that input.
- Tooltip and node graph docs mention both.

**Out:**
- Changing arrow layout or drawing: none (user chose the picker only).
- Dragging an input arrow onto a node: unchanged, since a node has one output.
- Shader-graph drop lists: M351 - Shader Graph Presentation's own.

**Edge cases:**
- Picking an input whose connection would form a cycle or fails the kind check: the usual `handleConnectionError` message, and nothing changes.
- Picking an already-connected input: replaces the connection, as dropping on an occupied arrow does.
- Inputs hidden by `isInputRotoBrush`: excluded from the list.
- A node with one input: Ctrl+drop connects directly, with no popup.
- Ctrl is already read on release (`modCASIsControl` counts a Ctrl release as a move): only a release on a node body changes, and a Ctrl release elsewhere behaves as today.

## Phase 491.1: Picker

- [ ] M491.P1.T1 — Add the input picker popup and Ctrl+drop
  - files: `Gui/NodeInputPicker.{h,cpp}` (new), `Gui/NodeGraph15.cpp`, `Gui/CMakeLists.txt` or the Gui sources list, `Tests/gui/extra_input_picker.py`
  - approach: a small `QMenu`-based popup built from the node's inputs (`getInputLabel`, `isInputMask`, `isInputRotoBrush`, current sources). In `mouseReleaseEvent`'s output-edge branch, when Ctrl is held on a node body and the node has more than one eligible input, open the popup at the cursor instead of using the preferred input. Connect the picked one through `handleConnectionError` and `ConnectCommand`, as the existing branch does.
  - verify: an Xvfb script at 1920x1080 builds Read → RotoPaint, Ctrl+drops a second Read's output on RotoPaint's body, picks Vectors, and asserts the input connected while Source and Bg are unchanged; one undo restores the previous state; a plain drop still connects the preferred input. A screenshot of the open picker goes to the user.
  - size: M

- [ ] M491.P1.T2 — Add Connect Input to the node context menu
  - files: `Gui/NodeGraph35.cpp`, `Gui/NodeInputPicker.cpp`, `Tests/gui/extra_input_picker.py`, the node graph docs page under `Documentation/`
  - approach: when the right-click lands on a node and exactly one other node is selected, add a "Connect Input" submenu filled by `NodeInputPicker`'s list builder, connecting the selected node. Add the tooltip and doc text for both gestures.
  - verify: the same Xvfb script right-clicks RotoPaint with the Read selected, picks Vectors and asserts the connection and single-step undo; a screenshot of the menu goes to the user.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `Tests/gui/extra_input_picker.py` passes under Xvfb; the GUI bench rule (board Context) is run and recorded, since the diff touches `Gui/NodeGraph*`; the user OKs the screenshots.

## Decisions

- 2026-11-17 — **Picker only, opened by Ctrl+drop and right-click (user):** chosen over side stubs and a layout change, and over opening the list on every drop, so the common drop is unchanged.
