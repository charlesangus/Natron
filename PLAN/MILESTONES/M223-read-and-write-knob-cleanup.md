# M223 - Read And Write Knob Cleanup

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Follow-ups the user found during M167 - Per-Frame Node Metadata's UAT (2026-10-26):

- **Read shows two Reload buttons.** Find where each comes from (the native Read's own Reload and a generic/file-knob one, or the OFX reader's leftover) and keep exactly one.
- **Write renders its input's format, always.** Remove the Write's format-type knob (and the output-format/size knobs behind it): letting the Write set the render format is a misfeature; reformatting belongs upstream in a Reformat node. The Write always writes the input format.

Acceptance sketch:
- A Read panel has one Reload button, and it reloads.
- A Write has no format-type or format knobs; the written file's size is the input's format/RoD as today's "input format" mode; any code paths for the other modes are deleted, not hidden.
- Tests cover the Write's output size following the input; the Read's knob list has one Reload.
