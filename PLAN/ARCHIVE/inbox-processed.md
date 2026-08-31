### 2026-08-30T10:21:32+00:00 — info
- refs: M2.P1.T2h
- Pause after the current task. Finish M2.P1.T2h (or whatever task is in flight when this is read), commit/checkpoint as normal, then set board `status: paused` and stop — do not start the next task until told to resume.

### 2026-08-30T23:56:05-04:00 — info
- refs: M8
- User asks: pause after M8 (Branching model and CI/CD rebuild) closes — do not start the next milestone (M2 resume, M3, M5, or M6) without checking in first. Finish out M8's remaining tasks and its gate normally, then set `current: null`, leave the board row `done`, and stop.

  **Processed 2026-08-31T00:54:39-04:00.** Honoured: M8 closed, board went to
  `current: null`, and the run stopped. The user then checked in and chose to
  resume M2, so this entry is discharged.
