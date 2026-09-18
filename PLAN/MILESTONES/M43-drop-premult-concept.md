# Milestone 43: Drop the premultiplied/unpremultiplied concept

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Remove the app's built-in premultiplied/unpremultiplied tracking and
handling; treat that as the user's responsibility to manage/track, as in
other professional compositing software.

Blocked on: not yet prioritized — captured as backlog (user request
2026-09-18); needs a scoping pass on everywhere premult state is read or
written before elaboration, since it likely touches many nodes' metadata
handling.

Acceptance sketch:
- The app no longer tracks or exposes a premultiplied/unpremultiplied
  concept; existing projects still load sensibly.
