# M338 - Viewer Input Keys And Locks

Number keys behave predictably, and any viewer input can be locked to a node. With focus in the node graph and one node selected, `#` pipes that node to input `#` and activates it. With no usable selection, or with focus anywhere else, `#` only activates input `#`. Plain number keys never turn the wipe on. `Alt-#` locks an input to the selected node by name, so `#` always recalls that node whatever is selected, and `Alt-Shift-#` unlocks it.

The lock follows the user's `viewer_lock` project (github.com/charlesangus/viewer_lock). Docs, tooltips and comments do not mention the application it was written for.

**Cause of the stray wipe:** `ViewerTab::connectToInput` (`Gui/ViewerTab30.cpp`) sets `setActivateInputChangeRequestedFromViewer(true)`, and `ViewerTab::onActiveInputsChanged` then wipes when A and B differ and the operator is None. A first connection sets B equal to A, so going from input 1 to input 2 with focus in the viewer leaves A=2 and B=1 and turns the wipe on. The same key with focus in the graph takes a different path and does not wipe.

## Scope

**In:**
- Focus-based routing for `#` and `Shift-#`, on the A and B sides.
- Plain number keys never turn the wipe on; the `autoWipeForViewer` preference and its flag are removed.
- A node may feed several viewer inputs at once, unconditionally.
- `Alt-#` and `Alt-Shift-#` with rebindable shortcuts, per-input locks saved with the project, and the lock label and colour on the Viewer node.

**Out:**
- Setting B turning the wipe on: `Shift-#` and the B dropdown keep doing this (see Decisions).
- Locks following a node rename (see Decisions).
- Changing the proxy-level shortcuts, or Alt-# from the viewer or any other panel.
- Blocking manual pipe drags onto a locked input; the next press restores the locked node.
- Any legacy project handling (no backward compat in this fork).

**Edge cases:**
- More than one node selected, the viewer itself selected, or a selected node that others can't connect to: `#` activates only, as with no selection. `Alt-#` does nothing.
- `#` on an unconnected, unlocked input with nothing to pipe: no change.
- The selected node is already on input `#` but that input is not active: `#` activates it.
- `Shift-#` with a selection and graph focus pipes to input `#` and sets B. Today the side is dropped and A is set.
- Plate locked to 3, plate selected, graph focus, press 1: the plate goes to 1 and stays on 3.
- Locked input, any focus, any selection: `#` connects the locked node if its pipe differs, then activates. `Shift-#` does the same for B.
- Locked name no longer resolves (renamed or deleted): the next `#` clears that lock, then behaves as an unlocked key.
- `Alt-#` on an already locked input replaces the lock. `Alt-Shift-#` on an unlocked input does nothing. Unlock leaves the pipe and active inputs alone.
- Activate-only presses with no viewer do nothing; the graph path keeps creating a viewer as it does today.
- Each viewer has its own locks. Keys act on the viewer the graph already picks (last selected, in this graph).

**Assumptions:**
- Natron's focus-follows-mouse (`PanelWidget::enterEventBase`) makes "the graph has keyboard focus" match what the user means by focus. M338.P1.T2's GUI script checks this with the viewer and properties panels.
- `Node::connectInput`'s default `isInputA = true` means a dragged pipe or `#` piping sets A.

## Phase 338.1: Keys And Focus

- [ ] M338.P1.T1 — Plain number keys never turn the wipe on
  - files: Gui/ViewerTab30.cpp, Engine/ViewerInstance.{h,cpp}, Engine/Settings.{h,cpp}, Tests/gui/m338_viewer_input_keys.py
  - approach: remove the wipe branch in `ViewerTab::onActiveInputsChanged`, `setActivateInputChangeRequestedFromViewer`/`isInputChangeRequestedFromViewer` and the flag handling in `ViewerTab::connectToInput`, and the `autoWipeForViewer` knob, which then gates nothing. Keep the B-side wipe in `InspectorNode::refreshActiveInputs` (`Engine/NodeInputs.cpp`) and in `ViewerTab::onSecondInputNameChanged`. Start the GUI script, modelled on `Tests/gui/m55_node_graph.py`, sending real key events through `QApplication.sendEvent`.
  - verify: script: two connected inputs, viewer focused, press 1 and 2 repeatedly and the operator stays None; `Shift-2` and picking B in the dropdown still set Wipe Under; W toggles it. Fails on current main.
  - size: M
- [ ] M338.P1.T2 — Route number keys by graph focus
  - files: Gui/NodeGraph25.cpp, Gui/NodeGraph30.cpp, Gui/NodeGraph.h, Gui/Gui30.cpp, Tests/gui/m338_viewer_input_keys.py
  - approach: add an A and B digit branch to `NodeGraph::keyPressEvent`, calling `connectCurrentViewerToSelection`; only that path pipes. `Gui::connectInput` (uncaught keys from other panels, and the `actionConnectInput` actions) activates the last-focused graph's viewer through `ViewerTab::connectToInput` and never pipes. `ViewerTab::keyPressEvent` already activates only. In `connectCurrentViewerToSelection`, treat a non-connectable selected node as no selection, and after pushing `ConnectCommand` call `setActiveInputAndRefresh(inputNB, isASide)` so the B side is honoured and an already-piped input still activates. Use the same key matching as the existing `Shift-#` handling, since Shift can change the Qt key.
  - verify: script runs every combination of focus (graph, viewer, properties panel) and selection (none, one, two, viewer, non-connectable) on `#` and `Shift-#`, reading connected inputs and A/B from the viewer. Includes a nested Group graph and a press with the mouse moved from graph to viewer without clicking.
  - size: M

## Phase 338.2: Several Pipes From One Node

- [ ] M338.P2.T1 — Key the viewer input menus by input number
  - files: Gui/ViewerTab30.cpp, Gui/ViewerTab40.cpp, Gui/ViewerTabPrivate.h, Gui/ViewerTab.cpp, Tests/gui/m338_viewer_input_keys.py
  - approach: the A and B comboboxes find items by node label (`inputIndexForMenuText`, `itemIndex`, `removeItem` in `ViewerTab::onInputChanged` and `onInputNameChanged`), so duplicate labels collide. Map items to input numbers instead, including `switchInputAAndB` and `refreshInputMenusFromKnobs`. Duplicate labels get a suffix naming the input so the entries can be told apart. Note this collision already exists for two different nodes with the same label.
  - verify: script gives two nodes the same user label on inputs 1 and 2: picking each sets the right input, disconnecting one removes only its entry, renaming one relabels only its entry.
  - size: M
- [ ] M338.P2.T2 — Allow one node on several viewer inputs
  - files: Engine/NodeInputs.cpp, Gui/NodeGraphUndoRedo.cpp, Gui/NodeGraph10.cpp, Tests/gui/m338_viewer_input_keys.py
  - approach: remove the single-pipe rule in `InspectorNode::connectInput` (keep the same-input early return) and the disconnect loop in `ConnectCommand::doConnect`. Check the other `inputIndex` callers (`Engine/NodeInputs.cpp`, `Engine/NodeGroup.cpp`, `Gui/NodeGraph10.cpp`, `Gui/NodeGraphUndoRedo.cpp`), which return only the first match, against a viewer holding the same node twice, and fix any that would leave the second pipe dangling.
  - verify: script pipes one node to inputs 1 and 3, both activate and show it; undo and redo restore each; deleting the node clears both pipes, and undoing that restores both.
  - size: M

## Phase 338.3: Input Locks

- [ ] M338.P3.T1 — Per-input lock knobs on the viewer
  - files: Engine/ViewerInstance.{h,cpp}, Tests/ViewerKnobs_Test.cpp
  - approach: one hidden persistent string knob per input, built like the M164 knobs with `createViewerKnob` in `ViewerInstance::initializeKnobs`: no animation, no render on change, hash-exempt, secret by default, script names `lockedInput1`..`lockedInput10` with labels matching ("Locked Input 1"). Add getter and setter by input index. A lock holds the node's script name, resolved in the viewer's own collection.
  - verify: `Tests/ViewerKnobs_Test.cpp`: set, get, project.yaml round-trip, unchanged upstream hashes and viewer hash.
  - size: M
- [ ] M338.P3.T2 — Lock and unlock shortcuts
  - files: Gui/ActionShortcuts.{h,cpp}, Gui/GuiApplicationManager10.cpp, Gui/NodeGraph25.cpp, Gui/NodeGraph30.cpp, Gui/NodeGraph.h, Tests/gui/m338_viewer_input_keys.py
  - approach: register twenty `kShortcutGroupNodegraph` shortcuts, "Lock Viewer Input N" (`Alt-digit`, with 0 as input 10) and "Unlock Viewer Input N" (`Alt-Shift-digit`). Handle them only in `NodeGraph::keyPressEvent`, so `Alt-1`..`5` stay proxy levels when focus is in the viewer. Lock needs a single selected node that others can connect to and that isn't the viewer: store its script name, pipe it and activate A, with undo restoring both the lock and the pipe. Unlock only clears the knob. Note `Alt-Shift-1`..`5` also match the viewer's proxy shortcuts because Shift is masked; the graph handles these first when it has focus.
  - verify: script: `Alt-3` with a selection sets the knob, the pipe and A; undo restores them; `Alt-Shift-3` clears the knob and changes nothing else; with viewer focus `Alt-1` sets the proxy level and no lock; no selection does nothing.
  - size: M
- [ ] M338.P3.T3 — Number keys recall locked inputs
  - files: Gui/NodeGraph30.cpp, Gui/NodeGraph.h, Gui/ViewerTab30.cpp, Gui/Gui30.cpp, Tests/gui/m338_viewer_input_keys.py
  - approach: add one resolver, called before routing on every `#` and `Shift-#` path (graph, viewer, other panels). A locked input whose name resolves connects that node with `ConnectCommand` on the viewer's own graph if its pipe differs, then activates on the requested side. A name that doesn't resolve clears the lock and continues as unlocked. Both sides honour the lock; `viewer_lock`'s `Shift-#` skips it only because it looks up index + 10, which is not a behaviour to copy.
  - verify: script runs lock input 3, select another node, press 3 from each focus: pipe unchanged, input active; pipe dragged off input 3 then press 3: restored; plate on 3 selected, press 1: on both; renamed and deleted locked nodes: next press clears the lock and acts unlocked; lock survives save and reload.
  - size: M
- [ ] M338.P3.T4 — Lock label and colour on the Viewer node
  - files: Gui/NodeGraph30.cpp, Engine/ViewerInstance.cpp, Tests/gui/m338_viewer_input_keys.py
  - approach: as `viewer_lock`'s `update_colour_and_label`: on any lock change, including one cleared on press, rewrite only the `Locked input N to <name>` lines of the node's user label (`kUserLabelKnobName`), keeping the user's other lines. Colour the node dark grey (`0x111111`) while any lock exists, using `Node::setColor`, and restore with `Node::resetColor` when none remain. Lines use the input number as shown to the user, starting at 1.
  - verify: script checks the label and colour after lock, second lock, unlock, cleared-on-press and reload, with a user-written label line preserved. Screenshot of the graph shared with the user.
  - size: M

**Verification gate:** build via `tools/ci/remote/build.sh` (release and `debug --tests`, as PR CI runs debug asserts); `ViewerKnobs_Test` passes; `Tests/gui/m338_viewer_input_keys.py` passes under Xvfb; a graph screenshot of a locked Viewer is shared with the user; a release AppImage with a same-named steps `.md` passes the user's UAT covering the wipe, focus routing, several pipes, lock, unlock and save/reload.

## Decisions

- 2026-11-04 — Alt-# and Alt-Shift-# work only with graph focus (user): matches `viewer_lock`'s DAG-context shortcuts and keeps Alt+1..5 as viewer proxy levels.
- 2026-11-04 — Setting B still turns the wipe on (user): only plain number keys stop wiping, so `Shift-#` and the B dropdown keep it.
- 2026-11-04 — Remove the `autoWipeForViewer` preference: once the A-side path is gone it gates nothing, because the B-side wipe already ignores it. This is the planner's consequence of the user's choice, not something the user asked for directly.
- 2026-11-04 — A renamed locked node drops its lock at the next press (user): matches `viewer_lock`; no rename hook.
- 2026-11-04 — The single-pipe rule for viewer inputs is removed for every node, not just locked ones (user): there is no harm in two inputs sharing a node, so nothing should forbid it.
- 2026-11-04 — `Shift-#` honours locks too: the planner's reading of "pressing # doesn't change what it's piped to"; `viewer_lock`'s B side skips the lock only through an index offset.
