# M114 - Global Undo Correctness

Give Natron **one global undo history per project window that records every project edit**: knob changes, node graph edits, roto, tracker, curve editor and dope sheet edits, and preset loads. Ctrl+Z, Ctrl+Shift+Z and Edit → Undo/Redo always walk that one history in the order things happened, whichever panel has focus. A node's settings panel keeps its undo/redo buttons, but they become a **filtered view of the global history** (only the entries owned by that node), not a separate stack, so nothing is ever recorded only locally. User decision, 2026-10-12.

Today every node panel owns a private `QUndoStack` (`Gui/NodeGui.cpp:369`, registered at `Gui/NodeGraph.cpp:473-476`). So do each node graph (`Gui/NodeGraph.cpp:147-149`), the dope sheet (`Gui/DopeSheet.cpp:169,288`) and the curve widget (`Gui/CurveWidgetPrivate.cpp:100,115`). A `QUndoGroup` (`Gui/Gui05.cpp:85-86`, `Gui/Gui30.cpp:688-727`) chooses the active stack from mouse position and focus (`Gui/PanelWidget.cpp:86-121`, `Gui/DockablePanel.cpp:1594`, `Gui/PropertiesBinWrapper.cpp:83-95`).

The milestone also fixes the two preset bugs found in the M93 - Serialization Boundary UAT (2026-10-12). First, Load Presets is pushed onto the panel stack only (`Gui/NodeGui.cpp:2584`), where Ctrl+Z in the graph never reaches it. Second, applying a preset leaves alone every knob missing from the file (`Engine/Node.cpp:2424-2525`). The undo snapshot is just as sparse, so undo doesn't restore those knobs either.

## Scope

**In:**
- A GUI-free `UndoHistory` model: chronological entries, each with an owner (a node, the project, or none) and a touched-node set; global undo/redo; per-owner filtered undo/redo; merging; an undo limit; skipping obsolete entries.
- Routing every project-edit push site to it: `KnobGuiContainerHelper::pushUndoCommand` (`Gui/KnobGuiContainerHelper.cpp:1157`), `PanelWidget::pushUndoCommand` (`Gui/PanelWidget.cpp:107`), `NodeGui::pushUndoCommand` (`Gui/NodeGui.cpp:4436`, which carries engine `UndoCommand`s from roto and tracker), NodeGraph (top level and group graphs), DopeSheet, CurveWidget, MultiInstancePanel, RotoPanel, TrackerPanel and NodeViewerContext.
- Removing the per-node, graph, dope-sheet and curve stacks, the `QUndoGroup` and every `setActive()` focus switch. Also removing the dead `CurveEditor` stack (`Gui/CurveEditor.cpp:130-179,336`), which nothing pushes to.
- Knob commands that address engine knobs (`KnobIWPtr`) rather than `KnobGuiWPtr` (`Gui/KnobUndoCommand.h:359,390`), so history survives a knob GUI rebuild. Deleting `clearUndoRedoStack` and the `cmdBeingPushed`/`onDeleteCurCmdLater` workaround (`Gui/KnobGuiContainerHelper.cpp:1024,1183-1229`).
- Preset apply that resets every persistent knob absent from the preset to its default, with the agreed exceptions, in both redo and undo.

**Out:**
- Per-user undo, remote edits and node IDs: M106 - Live Session Presence And Undo / M105 - Live Session Core. This milestone only provides the entry metadata (touched-node set, obsolete skipping) that M106.P2.T1 builds on.
- Undo for edits made from Python or the Script Editor. Python edits stay non-undoable, and the Script Editor keeps its own text history (`Gui/ScriptEditor.cpp:78`).
- Preferences (`Gui/PreferencesPanel.cpp:403`) and PyPanel/PyModalDialog knobs (`Gui/PythonPanels.cpp:221,418`). These are not project edits, so they keep a private stack inside `KnobGuiContainerHelper`.
- Two-way links and Linked Nodes: M92 - Linked Nodes ports its new commands onto this model afterwards.
- Changing the preset file format, or what export writes.

**Edge cases:**
- Node deleted: its entries stay in the global history behind the delete entry, which holds the `NodeGui` in the trash. Undoing the delete brings the node back, and its panel history with it. Entries whose targets have died are marked obsolete (`QUndoCommand::setObsolete`) and skipped, not crashed on. The undo limit trims oldest first, so an entry never outlives the delete that follows it.
- Group nodes: entries made in a group's inner graph go to the same history. Deleting the group behaves like any other node delete.
- Panel undo out of order: a panel's undo is offered only when no later done entry touches that node. Graph-only commands (move, connect, resize backdrop, rearrange) touch no node's state and never block it.
- Merging (`QUndoCommand::id`/`mergeWith`, e.g. `Tests/ColorKnobUndoMerge_Test.cpp`): merge only into the newest global entry, and only when it has the same owner. `getLastUndoCommand` (`Gui/KnobGuiContainerHelper.cpp:1151`) returns that entry.
- A panel that doesn't exist yet: `NodeGui::pushUndoCommand` currently runs `redo()` with no undo entry when there is no panel. With this change it always records.
- Ctrl+Z while typing in a line edit or the Script Editor still undoes the text, because the widget consumes `ShortcutOverride`. Floating panes reach the global actions too (application shortcut context).
- Project close or load clears the history before any node is destroyed (today `NodeGraph::clear`, `Gui/NodeGraph.cpp:286`).
- Preset reset exceptions (pending the user decision): label, sublabel, disable, Read/Write file paths, and roto/tracker content absent from the preset are kept, not reset. The channel set is always written (`Engine/KnobSerializationConvert.cpp:569-573`), so it is not a special case. Multi-instance children are replaced wholesale, as today. User knobs the preset creates are removed on undo.

**Assumptions:**
- The `WriteNode` encoder swap the stub named no longer exists: M76 deleted `Engine/WriteNode.cpp`, and `KnobHolder::recreateKnobs` (`Engine/Knob.cpp:6194`), the only path to `clearUndoRedoStack`, has no callers. Knob GUIs are still destroyed by `recreateUserKnobs` (`Gui/KnobGuiContainerHelper.cpp:1099`; callers `Engine/PyNode.cpp:1114`, `Engine/Knob.cpp:5514`, `Gui/ManageUserParamsDialog.cpp:292`), which silently kills `KnobGuiWPtr` commands.
- Viewer overlay edits push through the knob GUI (`Engine/KnobImpl.h:1117-1126` requires a knob GUI pointer), so they exist only while the panel is open.
- `resetToDefaultValue` alone leaves expressions and links in place. P3.T1 verifies this and clears them explicitly.
- M16 - Project Format Redesign may delete or rename `GoldenXml_Test`; P3.T1 uses whichever project round-trip test exists at the time.

## Phase 114.1: One history, routed everywhere

- [x] M114.P1.T1 — Add the `UndoHistory` model with owner-filtered undo/redo, and unit-test it
  - files: `Gui/UndoHistory.h` (new), `Gui/UndoHistory.cpp` (new), `Gui/CMakeLists.txt`, `Tests/UndoHistory_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: A `QObject` that owns `QUndoCommand`s in push order, each entry holding `{done, owner (KnobHolder* weak), touched node set}`. `push(cmd, scope)` runs `redo()`, merges per the Edge-cases rule, and drops every undone entry. `undo()`/`redo()` are global; redo replays the most recently undone entry. `undo(owner)`/`redo(owner)` and `canUndo(owner)`/`canRedo(owner)` implement the panel rule. Also provides obsolete skipping, `setUndoLimit` (oldest first), `clear()`, `undoText()/redoText()` and a `changed()` signal. No widgets.
  - verify: `Tests/UndoHistory_Test.cpp` uses fake commands that append to a log. Cover global order across owners, panel undo of an older entry, panel undo blocked by a later entry touching the node, redo order, truncation on push, merge only with the newest entry, limit trimming and obsolete skipping. `tools/ci/remote/build.sh debug --tests`.
  - size: L

- [x] M114.P1.T2 — Make `Gui` own one history and drive Edit → Undo/Redo from it
  - files: `Gui/Gui.h`, `Gui/Gui05.cpp`, `Gui/Gui30.cpp`, `Gui/GuiPrivate.h`, `Gui/GuiPrivate.cpp`, `Gui/PanelWidget.h`, `Gui/PanelWidget.cpp`
  - approach: Replace `_undoStacksGroup`, `registerNewUndoStack`, `removeUndoStack`, `onCurrentUndoStackChanged` and `setUndoRedoActions` with `Gui::getUndoHistory()` and two permanent Edit-menu actions (Ctrl+Z / Ctrl+Shift+Z, `Qt::ApplicationShortcut`) whose text and enabled state follow `changed()`. Set the limit from `getMaximumUndoRedoNodeGraph()`; renaming that setting is allowed (clean break). `PanelWidget::pushUndoCommand` pushes to the history with owner none. Delete the `setActive()` in `enterEventBase`.
  - verify: covered by the P1.T5 Xvfb script; compiles in the phase build with P1.T3 and P1.T4.
  - size: M

- [x] M114.P1.T3 — Route the node graphs, dope sheet and curve widget to the history, and delete their stacks
  - files: `Gui/NodeGraph.cpp`, `Gui/NodeGraphPrivate.h`, `Gui/NodeGraphPrivate.cpp`, `Gui/DopeSheet.cpp`, `Gui/DopeSheet.h`, `Gui/CurveWidgetPrivate.cpp`, `Gui/CurveWidgetPrivate.h`, `Gui/CurveWidget.cpp`, `Gui/CurveEditor.cpp`, `Gui/CurveEditor.h`, `Gui/DopeSheetEditor.cpp`, `Gui/PropertiesBinWrapper.cpp`
  - approach: Remove each private `QUndoStack` and its `getUndoStack()` override. Pushes go to the history with a scope. Add/Remove/Paste/Group/Inline/Extract/Declone/Rename/Disable/Enable touch their nodes. Move/Connect/Resize/Rearrange touch none. Dope-sheet and curve key commands touch the nodes owning the edited knobs, with an owner only when that is a single node. Delete the dead `CurveEditor` stack and `getUndoRedoActions`. Clear the history from project close.
  - verify: `rg -n 'QUndoStack|QUndoGroup' Gui` lists only `Gui/UndoHistory.*`, `Gui/ScriptEditor.cpp` and `KnobGuiContainerHelper`'s private stack; then the P1.T5 script.
  - size: M

- [x] M114.P1.T4 — Make node panels a filtered view: route panel and engine commands with an owner, and drive the buttons from the history
  - files: `Gui/KnobGuiContainerHelper.h`, `Gui/KnobGuiContainerHelper.cpp`, `Gui/DockablePanel.cpp`, `Gui/NodeGui.h`, `Gui/NodeGui.cpp`, `Gui/NodeGraph.cpp`
  - approach: Nodes and the project push to the history with owner = holder. Preferences and PyPanels keep a private stack (constructor flag). `DockablePanel::onUndoClicked`/`onRedoPressed` (`:879-903`) call `undo(owner)`/`redo(owner)`, and the buttons refresh on `changed()`. Delete `focusInEvent`'s `setActive`. Delete `NodeGui::_undoStack`, `getUndoStack` and `removeUndoStack`, plus the registration at `NodeGraph.cpp:473-476`. `NodeGui::pushUndoCommand` and `restoreInternal` push with owner = node whether or not the panel exists.
  - verify: the P1.T5 script.
  - size: L

- [x] M114.P1.T5 — Xvfb test of global order, focus independence and panel buttons
  - files: `Tests/gui/m114_global_undo.py` (new)
  - approach: Follow `Tests/gui/m55_input_pipes.py`'s `trigger_undo` (trigger the Edit-menu action). Edit a knob on A in its panel, move B in the graph, key a curve in the curve editor, edit a dope-sheet key, connect A→B, and add a roto shape. Then, with the mouse over each pane in turn, undo all of them and check reverse order, then redo all. Press A's panel undo button with B edited afterwards: only A reverts. Delete A, undo the delete, and check that A's panel buttons still step its edits.
  - verify: `Tests/gui/run-gui-test.sh Tests/gui/m114_global_undo.py <fast AppImage>` exits 0.
  - size: M

## Phase 114.2: Commands outlive their widgets

- [x] M114.P2.T1 — Make knob undo commands address engine knobs
  - files: `Gui/KnobUndoCommand.h`, `Gui/KnobUndoCommand.cpp`, `Gui/KnobGui.h`, `Gui/KnobGui.cpp`, `Gui/KnobGui20.cpp`
  - approach: `KnobUndoCommand<T>`, `MultipleKnobEditsUndoCommand` and `PasteUndoCommand` store `KnobIWPtr`. They do their value and keyframe work on the knob, and look up `knob->getKnobGuiPointer()` only for optional refresh. If the knob is dead, the command goes obsolete. Constructors may still take a `KnobGuiPtr`. Keep the merge semantics (`Tests/ColorKnobUndoMerge_Test.cpp`, `Tests/KnobUndoHash_Test.cpp` stay green).
  - verify: `tools/ci/remote/build.sh debug --tests` and the P2.T3 script.
  - size: L

- [x] M114.P2.T2 — Audit every other command for widget pointers that die on rebuild, and re-key them
  - files: `Gui/KnobGuiValue.cpp` (`DimensionsFoldCommand`), `Gui/ColorKnobEditSession.cpp`, `Gui/CurveEditorUndoRedo.h`, `Gui/CurveEditorUndoRedo.cpp`, `Gui/DopeSheetEditorUndoRedo.cpp`
  - approach: Wherever a command holds a `KnobGui`, `CurveGui` or `DSKnob` and only needs the knob, re-key it to the knob, resolving the view object at execution time. List the commands left untouched, and why (`NodeGraphUndoRedo`, `MultiInstancePanel`, `RotoPanel` and Roto/Tracker hold nodes or items, not knob GUIs), in the PR.
  - verify: `tools/ci/remote/build.sh debug --tests`; P2.T3 covers a curve key edit across a rebuild.
  - size: M

- [x] M114.P2.T3 — Delete the stack-clearing workaround, and test history across a knob GUI rebuild
  - files: `Gui/KnobGuiContainerHelper.h`, `Gui/KnobGuiContainerHelper.cpp`, `Tests/gui/m114_rebuild_undo.py` (new)
  - approach: Remove `clearUndoRedoStack`, `cmdBeingPushed`, `clearedStackDuringPush`, `onDeleteCurCmdLater` and the call in `refreshGuiForKnobsChanges`. Test: edit a plugin knob and a user knob, add a user knob via Manage User Params (`recreateUserKnobs`), key a knob in the curve editor, then undo everything in order and redo.
  - verify: `tools/ci/remote/build.sh fast`, then `Tests/gui/run-gui-test.sh Tests/gui/m114_rebuild_undo.py <AppImage>` exits 0.
  - size: M

- [x] M114.P2.T4 — Record edits to knobs that do not trigger a render
  - files: `Gui/KnobGui.cpp`, `Gui/KnobUndoCommand.cpp`, `Tests/UndoHistory_Test.cpp`, `Tests/gui/m114_global_undo.py`
  - approach: `KnobGui::pushUndoCommand` (`Gui/KnobGui.cpp` ~:171) applies and discards the command for a knob that cannot be undone or does not trigger a render, so a node label, the label's font size, a note or any other non-render project knob never reaches the history. Push every user edit of a persistent project knob into the global history whatever its render flag; keep the discard only for knobs that are genuinely not undoable (buttons, transient UI-only knobs). Add one Xvfb check that changes Blur1's label font size from the Node page and undoes it with Ctrl+Z.
  - verify: the phase build; the new unit case and the GUI check pass.
  - size: M

## Phase 114.3: Exact preset apply and undo

- [x] M114.P3.T1 — Add a preset-only apply that resets absent persistent knobs
  - files: `Engine/Node.h`, `Engine/Node.cpp`, `Engine/NodeMain.cpp`, `Tests/NodePresetApply_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: `Node::applyPresetSerialization(const NodeSerialization&, std::list<KnobIPtr>* createdUserKnobs)`. For each persistent, non-page/group/button plugin knob absent from the file and not on the exception list: remove animation, clear expression and links, `resetToDefaultValue`. Then call `fromSerialization`. Report which user knobs `restoreUserKnobs` created. `fromSerialization` itself is unchanged (project load, paste and clone share it).
  - verify: the engine gtest covers: a preset from a default node resets a modified target, including keyframes, an expression and a link; exception knobs are kept; sparse preset files work; applying the target's own full snapshot restores it exactly; the project round-trip test stays green. `tools/ci/remote/build.sh debug --tests`.
  - size: L

- [x] M114.P3.T2 — Use it in `LoadNodePresetsCommand` for both redo and undo
  - files: `Gui/NodeGraphUndoRedo.h`, `Gui/NodeGraphUndoRedo.cpp`, `Gui/NodeGui.cpp`, `Gui/NodeSettingsPanel.cpp`
  - approach: Redo and undo (`Gui/NodeGraphUndoRedo.cpp:1242-1330`) both go through `applyPresetSerialization` (new data, then the `serializeInternal` snapshot), keeping the links restore. Undo deletes the user knobs that redo created. The scope is owner = node, with that node and its multi-instance children touched.
  - verify: the P3.T3 script.
  - size: M

- [x] M114.P3.T3 — Xvfb preset test
  - files: `Tests/gui/m114_presets.py` (new)
  - approach: Export a preset from a mostly-default Blur. Modify a target Blur: values, a keyframe, an expression and a link to another node. Import the preset, check the defaults, then Ctrl+Z with the mouse over the graph and check that every value, expression and link is back. Redo re-applies it, and the panel buttons do the same. Repeat with a native Write preset and a preset that carries a user knob.
  - verify: `Tests/gui/run-gui-test.sh Tests/gui/m114_presets.py <fast AppImage>` exits 0.
  - size: M

## Phase 114.5: Python edits join the history

- [ ] M114.P5.T1 — Engine undo-recorder interface, guards and suppression points
  - files: `Engine/UndoRecorder.h` (new), `Engine/AppInstance.h`, `Engine/AppInstance.cpp`, `Engine/ProjectPrivate.cpp`, `Engine/OutputSchedulerThread.cpp`, `Engine/CMakeLists.txt`
  - approach: `UndoRecorderI` (openStep/closeStep nesting, isRecording, record(UndoCommandPtr, touched), suppression depth) plus `UndoStepGuard`/`UndoSuppressGuard`, no-ops without a recorder. `AppInstance::setUndoRecorder/getUndoRecorder` (non-virtual, null in background). isRecording is false off the main thread, while loading the project or creating a node tree, and while suppressed. Suppress in loadPythonScript (`AppInstance.cpp` ~:725), executeCommandLinePythonCommands (~:543), execOnProjectCreatedCallback (~:2301), createNodeFromPythonModule (~:871), the project save/close/load hooks (`ProjectPrivate.cpp` ~:366,447,496) and runCallbackWithVariables (`OutputSchedulerThread.cpp` ~:2002). Open a step in AppInstance::createNode when AddUndoRedo is set, so onNodeCreated (`NodeMain.cpp` ~:217) joins the creation step.
  - verify: compiles; covered by T6.
  - size: M

- [ ] M114.P5.T2 — Engine undo commands for Python edits
  - files: `Engine/PythonUndoCommands.h` (new), `Engine/PythonUndoCommands.cpp` (new), `Engine/CMakeLists.txt`
  - approach: Engine `UndoCommand`s holding weak nodes and knobs. NodesRemovedCommand: deactivate/activate as `RemoveMultipleNodesCommand` does (`Gui/NodeGraphUndoRedo.cpp` ~:289-470), with autoReconnect, destroying in the destructor once redone. NodeCreatedCommand: the mirror. ConnectInputCommand. RenameNodeCommand: script name plus label. KnobStateCommand: before copy via `createDetachedKnobFromSerialization` (`Engine/Knob.h` ~:3217), after copy taken at step close, applied with clone plus expressions and links. UserKnobsCommand: the holder's user knobs and page order, before and after. NodeLayoutCommand: position, size and colour. Each reports its touched holders.
  - verify: T6.
  - size: L

- [ ] M114.P5.T3 — Steps, capture frames and an engine-command adapter in the global history
  - files: `Gui/UndoHistory.h`, `Gui/UndoHistory.cpp`, `Gui/PythonUndoRecorder.h` (new), `Gui/PythonUndoRecorder.cpp` (new), `Gui/GuiAppInstance.cpp`, `Gui/CMakeLists.txt`, `Tests/UndoHistory_Test.cpp`
  - approach: Add openStep/closeStep/recordApplied to UndoHistory. An open step absorbs pushes and records, and closes into one entry: the union of touched holders, owned via ownByOnlyTouched, dropped if empty. In push(), the first redo opens a capture frame; captured records become trailing children of a compound entry. Redo runs outer then children; undo runs outer then children in reverse. mergeWith merges the outer and folds children by knob. During compound replay, set an app flag that skips Python callbacks (`EffectInstance.cpp` ~:5689-5692, `NodeInputs.cpp` runInputChangedCallback), since the children reproduce what they did. The recorder wraps engine commands as `NodeUndoRedoCommand` does (`Gui/NodeGui.cpp` ~:4445); GuiAppInstance installs it.
  - verify: new UndoHistory_Test cases: a step is one entry, an empty step is dropped, nested steps, capture into a push, no capture during replay, merge with children, undo limit counts steps.
  - size: L

- [ ] M114.P5.T4 — Record from the Python API
  - files: `Engine/PyNode.cpp`, `Engine/PyAppInstance.cpp`, `Engine/PyNodeGroup.cpp`, `Engine/PyParameter.cpp`
  - approach: When isRecording: `Effect::destroy` (~:278) pushes NodesRemovedCommand instead of destroyNode; connectInput/disconnectInput, setScriptName/setLabel, setPosition/setSize/setColor, create*Param/removeParam/setPagesOrder record. `App::createNode`/createReader/createWriter (`PyAppInstance.cpp` ~:161+) record NodeCreatedCommand and force AddUndoRedo off. Every mutating Param method (setValue*/set*, deleteValueAtTime, removeAnimation, setExpression, slaveTo/unslave/setAsAlias, parametric curves, restoreDefault) uses an RAII knob guard (first-touch before snapshot per step). Roto/tracker item structure is out.
  - verify: T6, T7.
  - size: L

- [ ] M114.P5.T5 — Bracket the GUI Python entry points
  - files: `Gui/ScriptEditor.cpp`, `Gui/Gui50.cpp`, `Gui/PythonPanels.cpp`, `Gui/KnobGuiButton.cpp`, `Gui/KnobGui.cpp`
  - approach: UndoStepGuard in onExecScriptClicked (~:460, step text "Script Editor"), onUserCommandTriggered (`Gui50.cpp` ~:984, the menu label), the Python clipboard paste (~:546), DialogParamHolder::onKnobValueChanged (`PythonPanels.cpp` ~:102), the trigger() calls (`KnobGuiButton.cpp` ~:209,212) and the discard branch of `KnobGui::pushUndoCommand` (`KnobGui.cpp` ~:187-190).
  - verify: T7.
  - size: S

- [ ] M114.P5.T6 — Engine gtest against a test recorder
  - files: `Tests/PythonUndo_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: In the style of `Tests/NodePresetApply_Test.cpp`: install a recorder that collects UndoCommandPtrs, run snippets with interpretPythonScript, then undo in reverse. Cover: destroy keeps the Node alive and undo reconnects it; createNode undo/redo; connect, rename, value/keyframe/expression/link restore exactly; a 100-key loop gives one KnobStateCommand; create/removeParam round-trip; nothing is recorded without a step, under suppression, or inside a PyPlug creation; background app: destroy really destroys.
  - verify: `tools/ci/remote/build.sh debug --tests`.
  - size: M

- [ ] M114.P5.T7 — Xvfb Python-undo script
  - files: `Tests/gui/m114_python_undo.py` (new)
  - approach: Create Blur1 with CreateNodeArgsPropAddUndoRedoCommand=True and type a size in its panel. Run `app.Blur1.destroy()` from the Script Editor (set text, trigger exec). Undo (Edit menu action, since Ctrl+Z in the Script Editor undoes its text): Blur1 is back with its outputs. Undo: the size reverts. Redo twice. Run a Script Editor multi-edit (create, connect, rename, setValue, setExpression, setValueAtTime x3); one undo reverts all of it, one redo restores it. Callback merge: an onParamChanged callback that sets a second knob; a panel edit then one undo reverts both. A button callback is one step.
  - verify: `Tests/gui/run-gui-test.sh Tests/gui/m114_python_undo.py <AppImage>` exits 0.
  - size: M

**Verification gate:** `tools/ci/remote/build.sh debug --tests` passes all of ctest, including `PythonUndo_Test` and the extended `UndoHistory_Test`. `m114_python_undo.py`, `m114_global_undo.py`, `m114_rebuild_undo.py` and `m114_presets.py` exit 0 on the AppImage. P4.T1's UAT checklist gains the user's example and a Script Editor multi-edit.

## Phase 114.4: Gate and UAT

- [ ] M114.P4.T1 — GUI bench, AppImage and UAT checklist
  - files: `build/appimages/M114-uat.md`
  - approach: If M117 has landed, run `tools/bench/gui/run.sh --xvfb` and `compare.py` against `tools/bench/gui/baselines/xvfb.jsonl`. Package an AppImage. The checklist covers all editors, panel buttons, node delete/undo and presets. Share Xvfb screenshots.
  - verify: the user signs off the UAT on the AppImage.
  - size: M

**Verification gate:** `tools/ci/remote/build.sh debug --tests` passes all of ctest, including `UndoHistory_Test`, `NodePresetApply_Test`, `ColorKnobUndoMerge_Test` and `KnobUndoHash_Test`. `m114_global_undo.py`, `m114_rebuild_undo.py` and `m114_presets.py` exit 0 under `Tests/gui/run-gui-test.sh` against the AppImage. `rg 'QUndoGroup|registerNewUndoStack|clearUndoRedoStack' Gui` finds nothing. The GUI bench shows no regression (once M117 has landed). The user signs off the UAT.

## Decisions

- 2026-10-25 — **UAT round 2 (user):** everything passes except panel undo order: three separate edits of one knob (mix .5, .25, .1) undo in one step back to 1, while interleaved knobs unwind correctly. Cause: same-knob commands merge across separate edits. Rule (PM): same-knob commands merge only within one continuous gesture (drag, scrub, held key, wheel); separate commits never merge. Fix in progress.
- 2026-10-24 — **Phase 5 implementation calls (PM):** a compound entry undoes its parts newest first with the pushed command last (strict LIFO), not outer-first as the plan said, since later parts were applied on top of it. Python createNode records by default with AddUndoRedo off, but an explicit AddUndoRedo=True gets the node graph command and is not recorded twice. Not recorded: toolset and PyPlug inner scripts (node-tree creation), roto item knobs, PyPanel knobs.
- 2026-10-24 — **Phase 114.5 design (consultant, PM):** record at the Python API layer (Engine/Py*.cpp) through an Engine-side UndoRecorderI that GuiAppInstance installs, not at engine entry points that GUI commands and plugins share. A Python delete deactivates the node (destroyed when the entry dies), so later entries on it still resolve. Callbacks are skipped while a compound entry replays, since its recorded children reproduce them. Roto/tracker item structure from Python is not recorded. Edits made from PySide widget handlers in Python panels have no C++ entry point and stay unrecorded unless a Python begin/end-undo-step API is added later.
- 2026-10-24 — **Python edits join the global history (user):** a Python run (Script Editor, Python panel or button callback, menu or shortcut command, PyPlug creation) is one undo step holding every node creation, deletion, connection, rename and knob value, expression or animation change it makes; callbacks fired by a user edit merge into that edit. Nothing is recorded during project load, project hooks, renders, command-line or background runs. The obsolete-entry checks from the review stay as a backstop. New Phase 114.5, before merge (user).
- 2026-10-24 — **Phase 3 gate passed (PM):** release 409/409 both legs on 2a08749f5; m114_presets, m114_global_undo and m114_rebuild_undo all 0 failures on the AppImage. Open points carried: links to missing nodes are skipped; open Roto/Tracker panels do not refresh on preset apply; multi-instance children created by a preset are outside its undo scope. GUI bench skipped until M117 lands.
- 2026-10-24 — **Roto context knobs default to disabled (PM):** the snapshot-undo gtest found that loading roto content flipped the default enabled state of the selection-driven Roto knobs, so a new node and a loaded one serialized differently. Defaults are now off at creation as on load (2a08749f5). Preset mode also keeps a Precomp `projectFilename` (user decision). Follow-up: preset mode applies the preset carried enabled flags for these knobs, which can disagree with the target selection until it changes.
- 2026-10-24 — **Phase 2 verified (PM):** release build on f389b8f3c is 391/391 on both legs. On that AppImage `m114_global_undo.py` has 0 failures, including the new label-font-size section; `m114_rebuild_undo.py` has 0 failures (setup 6, edits 12, rebuild 3, after-rebuild 4, undo-redo 14). Both script fixes were test-side. PySide invalidates `findChildren` wrappers when their parent panel's wrapper is collected, so the script types while it holds the panel. Window shortcuts only fire once the main window is active again after typing into a panel. Commands left untouched in P2.T2: node graph, MultiInstancePanel, RotoPanel and the engine roto/tracker commands hold nodes or items; NodeGui's command wraps an engine command; the script editor's holds only the editor; restore-defaults and set-expression already address the knob; the colour edit session pushes the knob value command.
- 2026-10-24 — **Phase 1 verified; non-render knob edits join Phase 2 (PM):** fast build on ea293de30 is 390/390 on both legs, and `m114_global_undo.py` (script 3b06c630f) reports 0 failures against that AppImage. The script needed only test fixes: a viewer for an empty project, activating the window before the graph drag, live curve-editor and dope-sheet widgets, and typing into a visible parameter. Its first typing target was the hidden label font-size box on the Node page, and that edit never reached the history because `KnobGui::pushUndoCommand` (`Gui/KnobGui.cpp` ~:171) discards commands for knobs that do not trigger a render. That predates M114, but it breaks the milestone's goal that every project edit is recorded, so P2.T4 fixes it.
- 2026-10-24 — **User decisions:** panel undo/redo buttons are a filtered view of the global history (a panel undo is allowed when no later entry touches that node; graph moves and connections don't block it). A preset apply resets absent knobs to default except: label, sublabel and disable; Read/Write file paths; roto/tracker content absent from the preset; and the target's own user knobs (undo removes user knobs the preset added).
- 2026-10-24 — **Elaborated from the stub (PM, opus consultant).** The stub's `WriteNode` encoder-swap case is gone (M76 deleted `Engine/WriteNode.cpp`); knob-GUI rebuilds through `recreateUserKnobs` still lose history, which Phase 114.2 fixes. The split with M92 - Linked Nodes stays: M114 changes how commands are pushed and what they address; M92's new link commands should address knobs (P2.T1) and list every node they touch. M106.P2.T1 should reuse this milestone's touched-node sets and obsolete skipping.
