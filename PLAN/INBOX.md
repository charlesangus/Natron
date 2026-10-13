## Pending

### 2026-10-12T00:00:00Z — change-request
- refs: M96
- From the M69 - Minor Cleanup UAT (user, 2026-10-12), add this to M96 - Weighted Curve Tangents' acceptance sketch. In bezier mode a curve-editor handle has two states. **Unbroken:** the in and out handles stay aligned, and changing one handle's length changes the other's too. **Broken:** each handle's length and angle change independently of the other. Weighted handles must also change the curve, not snap back on release. The user decided M69 does not implement length; M69 instead drops its visual-only follow-the-cursor handle drawing.

### 2026-10-12T00:00:01Z — change-request
- refs: none
- A follow-up from the M69 - Minor Cleanup UAT (user, 2026-10-12): "we shouldn't be 'copying' things when the node is in single-value mode, it should be referenced." Investigate, and possibly correct, the architecture of a folded (single-value) multi-component knob. Today folding only equalizes the current value, and M69.P10.T10 makes curve-editor edits copy dimension 0's curve to every dimension. The user wants a folded knob to hold one value or curve that the dimensions reference. Candidate homes are M31 - Architectural Cleanup and M95 - Knob Cleanup And Keyer Redesign.

### 2026-10-12T00:00:02Z — info
- refs: none
- Left over after M45 - Native Tab Menu merged: `Tests/gui/m76_write.py:181,183` still look up `gt.widgets_of("NodeCreationDialog")`. That class is gone (the Tab menu is now tabtabtab, and the completer was renamed `CompleterLineEdit`), so those lines of the M76 - Native Write GUI script will no longer find the node menu. They need the `m75_read.py` treatment: drive `TabTabTabWidget`. No milestone owns this yet.
