# M590 - Non-Modal Notifications

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

A message/notification system that presents important information without modal dialogs: load warnings, failed saves, missing plug-ins and similar results are shown in a non-blocking place (for example a notification area with a count, history and a link to the error log) instead of a pop-up that blocks the GUI and scripts.

Origin: user, 2026-11-24, while fixing M373 - In-Panel Knob Editor's GUI test that hung on the project load-warnings summary dialog planned in M16. Decision: keep that dialog modal for now; this milestone replaces it and the other modal informational dialogs.

Acceptance sketch:
- Loading a project with warnings shows them non-modally; `app.loadProject` returns without user interaction.
- Informational modal dialogs (warning/info summaries) are inventoried and moved to the notification system; confirmations that need an answer are out of scope unless the user says otherwise.
- Notifications persist for the session and are reachable after they fade.
