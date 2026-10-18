# M138 - GUI Papercuts

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Three small GUI fixes reported on 2026-10-18:
- Clicking the `>` right-arrow button in the file picker (`Gui/SequenceFileDialog.cpp`) crashes Natron.
- The splash screen and the Preferences window open on the main monitor, not on the monitor the user is on.
- Ctrl-click colour picking in the viewer should default to reading the viewer's displayed image. It should default to the node input only while a node is picking a colour for a knob, that is, after the swatch's eyedropper has been picked up. This changes the input/output selection that M69 - Minor Cleanup settled.

Blocked on: nothing technical; it needs elaborating (reproduce the crash first, and decide what "current monitor" means for the splash, which opens before any window exists).

Acceptance sketch:
- The `>` button in the file picker no longer crashes, with a regression test or GUI script.
- On a multi-monitor setup the splash and Preferences open on the monitor under the cursor or the active window.
- Outside knob picking, Ctrl-click reads the viewer's displayed image; during knob picking it defaults to the input.
