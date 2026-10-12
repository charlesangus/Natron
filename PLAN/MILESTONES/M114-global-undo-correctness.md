# M114 - Global Undo Correctness

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Give Natron **one global undo stack that tracks every user edit**: knob changes, node graph edits, roto and curve edits, preset loads, and anything else that changes the project. Ctrl+Z and Edit → Undo should always walk that one history in the order things happened, whichever panel has focus. Nodes **keep their own local undo stacks** so the undo/redo buttons on a node's settings panel still step through that node's edits. A local stack becomes a per-node view into, or a mirror of, the global history; it is never the only record of an edit. User decision, 2026-10-12.

Today each node's settings panel has a private `QUndoStack` (created at `Gui/NodeGui.cpp:~328`, registered at `Gui/NodeGraph.cpp:~453`). The node graph has its own stack, and `PanelWidget::enterEventBase` (`Gui/PanelWidget.cpp:~86`) switches the active stack according to where the mouse is. `KnobGuiContainerHelper::pushUndoCommand` (`Gui/KnobGuiContainerHelper.cpp:~1147`) pushes knob edits onto the panel stack. As a result, an edit made in a panel disappears from Ctrl+Z as soon as the mouse enters the node graph, and edits in different stacks are never ordered against each other.

Found in the M93 - Serialization Boundary UAT (2026-10-12). Both problems are **pre-existing** (`origin/main` behaves the same way) and are in scope here:
1. **Applying a node preset isn't undoable in practice.** Settings panel → **Import presets** → `NodeGui::restoreInternal` (`Gui/NodeGui.cpp:~2468`) pushes `LoadNodePresetsCommand` (`Gui/NodeGraphUndoRedo.cpp:~1152-1233`) onto the panel stack only, where Ctrl+Z in the node graph never reaches it. When the load rebuilds the panel's knobs (`KnobGuiContainerHelper.cpp:~1014`, `clearUndoRedoStack`; the `WriteNode` encoder swap does this), the command is dropped entirely.
2. **A preset only sets the knobs that were non-default on the exporting node.** Export saves only knobs with `hasModificationsForSerialization()` (or an expression or link), and apply leaves every knob missing from the file untouched (`Node::loadKnob`). The undo snapshot is equally sparse, so undo doesn't reset knobs the preset moved away from default. Preferred fix: before applying, reset every persistent knob absent from the preset to its default, in both `redo` and `undo` (`RestoreDefaultsCommand`, `Gui/KnobUndoCommand.cpp:~686`, is the model), so old `.nps` files work too. The reset must be limited to preset apply, because `Node::fromSerialization` is shared with project load, copy/paste and clone.

Blocked on: M93 - Serialization Boundary merging, since the preset fixes build on its plain `Serialization::NodeSerialization` and `Node::fromSerialization`. Elaboration also needs a design for the global stack:
- how local per-node stacks relate to it (filtered view versus mirrored commands);
- what happens to a node's history when the node is deleted;
- how to handle commands that rebuild knobs today and clear a stack;
- how this interacts with M106 - Live Session Presence And Undo, so the two designs don't diverge.

Acceptance sketch:
- Any edit, wherever it was made (panel, node graph, viewer, curve editor, roto), is undone and redone by Ctrl+Z / Ctrl+Shift+Z and Edit → Undo/Redo regardless of focus, in chronological order across all of them.
- A node panel's undo/redo buttons still step through only that node's edits, and stay consistent with the global history.
- Import a preset, then undo: the node is restored exactly, including knobs the preset changed from default, expressions and links. Redo re-applies it. Nothing is dropped when the preset rebuilds knobs.
- Applying a preset exported from a mostly-default node resets the target node's other knobs to default. Reset exceptions are decided and tested: label and sublabel, disable, channel set, Read/Write file paths, `WriteNode` encoder, multi-instance children, and roto and tracker contexts.
- Loading `.ntp` files, copy/paste and clone are unaffected (GoldenXml stays green).
