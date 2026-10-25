# M177 - Renders Isolated From Edits

Stub. No render other than the viewer may observe a knob edit made while it runs. Today an in-process Write render runs behind the non-modal progress panel, so the user can keep editing, and every edit reaches `EffectInstance::abortAnyEvaluation` (`Engine/EffectInstance.cpp`), which aborts it. Tracking and the colour sampler also run while the UI is live and read live knobs. "Render in separate process" (`Settings::isRenderInSeparatedProcessEnabled`, `Engine/AppInstance.cpp` ~:1893) already saves the project and renders a frozen copy in another process.

The user's model (2026-10-24): a foreground render blocks the UI behind a modal, so no edit can happen during it; a background render works from a frozen, immutable copy of the project captured at submission; tracking and the colour sampler block the UI like a foreground render. Only viewer renders restart on a knob change.

Filed from M167 - Per-Frame Node Metadata, Phase 3, which inherited M144's "edits during non-abortable renders" acceptance; the user ruled that such edits must be impossible rather than tolerated. Deps: M167 (same render-engine code; M167 drops its mid-render-edit test in favour of this milestone).

## Scope

**In (to confirm at elaboration):**
- Foreground Write renders (Render dialog, Write's Render button, Python `app.render` from the GUI) show a modal progress dialog that blocks editing until the render finishes or is cancelled.
- Background renders always render a frozen copy captured at submission. Default: the existing separate-process path (save the project, render it in `NatronRenderer`). Whether in-process graph cloning is ever wanted is a question for elaboration, not assumed.
- Tracking (track forward/backward, track range) and the colour sampler block the UI behind a modal with cancel, like a foreground render.
- `abortAnyEvaluation` aborts and restarts only viewer renders on an edit. Node deactivation and deletion still abort everything that reads the node.
- Tests: an edit attempted during a foreground render, track or sample is impossible (the modal owns input); a background render's output matches the project as submitted after edits in the GUI; viewer renders still restart on an edit.

**Out:**
- Per-frame metadata snapshots, which M167 owns.
- Command-line renders, which have no UI.

**Edge cases:**
- Python scripts that edit knobs from callbacks during a foreground render (e.g. a Write's before-frame callback): decide whether they are allowed and what they see.
- A modal render started from inside another modal (Preferences, a Python panel).
- Cancel from the modal mid-frame; partially written files.
- Tracking from the viewer's track buttons with the Tracker panel's keyboard shortcuts.

**Assumptions:**
- The separate-process background render already freezes the project at submission.

## Decisions

- 2026-10-24 — **Created at the user's request (user):** foreground renders block the UI; background renders work from a frozen copy captured at submission; tracker and colour sampler block the UI like a foreground render; only the viewer restarts on a knob change. Kept out of M167 at the user's request.
