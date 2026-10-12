# M114 - Authoritative Undoable Presets

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Make applying a node preset (`.nps`, settings panel → **Import presets**) work the way users expect. A preset should fully define the node's knob state, and applying it should be one undoable step in Edit → Undo. Both problems were found in the M93 - Serialization Boundary UAT (2026-10-12) and are **pre-existing**: `origin/main` behaves the same way.

The only way to apply a preset is `Gui/NodeSettingsPanel.cpp` `onImportPresetsActionTriggered` → `NodeGui::restoreInternal` (`Gui/NodeGui.cpp:~2468`), which pushes `LoadNodePresetsCommand` (`Gui/NodeGraphUndoRedo.cpp:~1152-1233`).

**1. Applying a preset doesn't appear in undo.** `LoadNodePresetsCommand` goes onto the node's private panel `QUndoStack` (`Gui/KnobGuiContainerHelper.cpp:~1147`), not the node graph's. Once the mouse enters the node graph, its stack becomes active (`Gui/PanelWidget.cpp:~86`), so Ctrl+Z and Edit → Undo never reach the preset load. The command is also dropped outright when the load rebuilds the panel's knobs (`KnobGuiContainerHelper.cpp:~1014`, `clearUndoRedoStack`), as `WriteNode`'s encoder swap does. Options:
- push the command onto the node graph stack;
- keep the panel stack but make it visible.

Either way, handle the knob-rebuild case so the command survives.

**2. A preset only sets the knobs that were non-default on the exporting node.** Export saves only knobs with `hasModificationsForSerialization()` (or an expression or link), and apply leaves every knob missing from the file untouched (`Node::loadKnob`). Undo has the same gap: the "before" snapshot is equally sparse, so undo doesn't reset knobs the preset changed from default. Options:
- **Preferred:** reset every persistent knob absent from the preset to its default before applying, in both `redo` and `undo`. `RestoreDefaultsCommand` (`Gui/KnobUndoCommand.cpp:~686`) is the model. Old `.nps` files then work too.
- **Alternative:** export every persistent knob into presets.

Blocked on: M93 - Serialization Boundary merging. Both fixes build on its plain `Serialization::NodeSerialization` and `Node::fromSerialization`, and the reset must stay limited to preset apply, because `fromSerialization` is shared with project load, copy/paste and clone. A design choice is also needed: which undo stack a preset load belongs on.

Acceptance sketch:
- Import a preset, then Edit → Undo (or Ctrl+Z in the node graph) restores the node exactly, including knobs the preset changed from default, expressions and links. Redo re-applies it.
- Applying a preset exported from a node with mostly default knobs resets the target node's other knobs to default.
- Reset exceptions are decided and tested: label and sublabel, disable, channel set, Read/Write file paths, `WriteNode` encoder (triggers a knob rebuild), multi-instance children, and roto and tracker contexts.
- Loading `.ntp` files, copy/paste and clone are unaffected (GoldenXml stays green).
