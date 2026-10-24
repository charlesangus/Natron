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

- [ ] M114.P2.T1 — Make knob undo commands address engine knobs
  - files: `Gui/KnobUndoCommand.h`, `Gui/KnobUndoCommand.cpp`, `Gui/KnobGui.h`, `Gui/KnobGui.cpp`, `Gui/KnobGui20.cpp`
  - approach: `KnobUndoCommand<T>`, `MultipleKnobEditsUndoCommand` and `PasteUndoCommand` store `KnobIWPtr`. They do their value and keyframe work on the knob, and look up `knob->getKnobGuiPointer()` only for optional refresh. If the knob is dead, the command goes obsolete. Constructors may still take a `KnobGuiPtr`. Keep the merge semantics (`Tests/ColorKnobUndoMerge_Test.cpp`, `Tests/KnobUndoHash_Test.cpp` stay green).
  - verify: `tools/ci/remote/build.sh debug --tests` and the P2.T3 script.
  - size: L

- [ ] M114.P2.T2 — Audit every other command for widget pointers that die on rebuild, and re-key them
  - files: `Gui/KnobGuiValue.cpp` (`DimensionsFoldCommand`), `Gui/ColorKnobEditSession.cpp`, `Gui/CurveEditorUndoRedo.h`, `Gui/CurveEditorUndoRedo.cpp`, `Gui/DopeSheetEditorUndoRedo.cpp`
  - approach: Wherever a command holds a `KnobGui`, `CurveGui` or `DSKnob` and only needs the knob, re-key it to the knob, resolving the view object at execution time. List the commands left untouched, and why (`NodeGraphUndoRedo`, `MultiInstancePanel`, `RotoPanel` and Roto/Tracker hold nodes or items, not knob GUIs), in the PR.
  - verify: `tools/ci/remote/build.sh debug --tests`; P2.T3 covers a curve key edit across a rebuild.
  - size: M

- [ ] M114.P2.T3 — Delete the stack-clearing workaround, and test history across a knob GUI rebuild
  - files: `Gui/KnobGuiContainerHelper.h`, `Gui/KnobGuiContainerHelper.cpp`, `Tests/gui/m114_rebuild_undo.py` (new)
  - approach: Remove `clearUndoRedoStack`, `cmdBeingPushed`, `clearedStackDuringPush`, `onDeleteCurCmdLater` and the call in `refreshGuiForKnobsChanges`. Test: edit a plugin knob and a user knob, add a user knob via Manage User Params (`recreateUserKnobs`), key a knob in the curve editor, then undo everything in order and redo.
  - verify: `tools/ci/remote/build.sh fast`, then `Tests/gui/run-gui-test.sh Tests/gui/m114_rebuild_undo.py <AppImage>` exits 0.
  - size: M

- [ ] M114.P2.T4 — Record edits to knobs that do not trigger a render
  - files: `Gui/KnobGui.cpp`, `Gui/KnobUndoCommand.cpp`, `Tests/UndoHistory_Test.cpp`, `Tests/gui/m114_global_undo.py`
  - approach: `KnobGui::pushUndoCommand` (`Gui/KnobGui.cpp` ~:171) applies and discards the command for a knob that cannot be undone or does not trigger a render, so a node label, the label's font size, a note or any other non-render project knob never reaches the history. Push every user edit of a persistent project knob into the global history whatever its render flag; keep the discard only for knobs that are genuinely not undoable (buttons, transient UI-only knobs). Add one Xvfb check that changes Blur1's label font size from the Node page and undoes it with Ctrl+Z.
  - verify: the phase build; the new unit case and the GUI check pass.
  - size: M

## Phase 114.3: Exact preset apply and undo

- [ ] M114.P3.T1 — Add a preset-only apply that resets absent persistent knobs
  - files: `Engine/Node.h`, `Engine/Node.cpp`, `Engine/NodeMain.cpp`, `Tests/NodePresetApply_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: `Node::applyPresetSerialization(const NodeSerialization&, std::list<KnobIPtr>* createdUserKnobs)`. For each persistent, non-page/group/button plugin knob absent from the file and not on the exception list: remove animation, clear expression and links, `resetToDefaultValue`. Then call `fromSerialization`. Report which user knobs `restoreUserKnobs` created. `fromSerialization` itself is unchanged (project load, paste and clone share it).
  - verify: the engine gtest covers: a preset from a default node resets a modified target, including keyframes, an expression and a link; exception knobs are kept; sparse preset files work; applying the target's own full snapshot restores it exactly; the project round-trip test stays green. `tools/ci/remote/build.sh debug --tests`.
  - size: L

- [ ] M114.P3.T2 — Use it in `LoadNodePresetsCommand` for both redo and undo
  - files: `Gui/NodeGraphUndoRedo.h`, `Gui/NodeGraphUndoRedo.cpp`, `Gui/NodeGui.cpp`, `Gui/NodeSettingsPanel.cpp`
  - approach: Redo and undo (`Gui/NodeGraphUndoRedo.cpp:1242-1330`) both go through `applyPresetSerialization` (new data, then the `serializeInternal` snapshot), keeping the links restore. Undo deletes the user knobs that redo created. The scope is owner = node, with that node and its multi-instance children touched.
  - verify: the P3.T3 script.
  - size: M

- [ ] M114.P3.T3 — Xvfb preset test
  - files: `Tests/gui/m114_presets.py` (new)
  - approach: Export a preset from a mostly-default Blur. Modify a target Blur: values, a keyframe, an expression and a link to another node. Import the preset, check the defaults, then Ctrl+Z with the mouse over the graph and check that every value, expression and link is back. Redo re-applies it, and the panel buttons do the same. Repeat with a native Write preset and a preset that carries a user knob.
  - verify: `Tests/gui/run-gui-test.sh Tests/gui/m114_presets.py <fast AppImage>` exits 0.
  - size: M

## Phase 114.4: Gate and UAT

- [ ] M114.P4.T1 — GUI bench, AppImage and UAT checklist
  - files: `build/appimages/M114-uat.md`
  - approach: If M117 has landed, run `tools/bench/gui/run.sh --xvfb` and `compare.py` against `tools/bench/gui/baselines/xvfb.jsonl`. Package an AppImage. The checklist covers all editors, panel buttons, node delete/undo and presets. Share Xvfb screenshots.
  - verify: the user signs off the UAT on the AppImage.
  - size: M

**Verification gate:** `tools/ci/remote/build.sh debug --tests` passes all of ctest, including `UndoHistory_Test`, `NodePresetApply_Test`, `ColorKnobUndoMerge_Test` and `KnobUndoHash_Test`. `m114_global_undo.py`, `m114_rebuild_undo.py` and `m114_presets.py` exit 0 under `Tests/gui/run-gui-test.sh` against the AppImage. `rg 'QUndoGroup|registerNewUndoStack|clearUndoRedoStack' Gui` finds nothing. The GUI bench shows no regression (once M117 has landed). The user signs off the UAT.

## Decisions

- 2026-10-24 — **Phase 1 verified; non-render knob edits join Phase 2 (PM):** fast build on ea293de30 is 390/390 on both legs, and `m114_global_undo.py` (script 3b06c630f) reports 0 failures against that AppImage. The script needed only test fixes: a viewer for an empty project, activating the window before the graph drag, live curve-editor and dope-sheet widgets, and typing into a visible parameter. Its first typing target was the hidden label font-size box on the Node page, and that edit never reached the history because `KnobGui::pushUndoCommand` (`Gui/KnobGui.cpp` ~:171) discards commands for knobs that do not trigger a render. That predates M114, but it breaks the milestone's goal that every project edit is recorded, so P2.T4 fixes it.
- 2026-10-24 — **User decisions:** panel undo/redo buttons are a filtered view of the global history (a panel undo is allowed when no later entry touches that node; graph moves and connections don't block it). A preset apply resets absent knobs to default except: label, sublabel and disable; Read/Write file paths; roto/tracker content absent from the preset; and the target's own user knobs (undo removes user knobs the preset added).
- 2026-10-24 — **Elaborated from the stub (PM, opus consultant).** The stub's `WriteNode` encoder-swap case is gone (M76 deleted `Engine/WriteNode.cpp`); knob-GUI rebuilds through `recreateUserKnobs` still lose history, which Phase 114.2 fixes. The split with M92 - Linked Nodes stays: M114 changes how commands are pushed and what they address; M92's new link commands should address knobs (P2.T1) and list every node they touch. M106.P2.T1 should reuse this milestone's touched-node sets and obsolete skipping.
