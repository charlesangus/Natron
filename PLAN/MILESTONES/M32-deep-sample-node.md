# Milestone 32: Deep sample inspector node (replaces the hover probe)

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

A Nuke-style `DeepSample` node: a native deep pass-through (identity on the
deep stream, never modifies it) with a position knob settable by typing or by
dragging a picker handle in the Viewer, and a settings-panel table listing
every raw sample (Z, ZBack, A, R, G, B, plus any AOVs present) at that
position for the current frame. Replaces M18.P2.T3's hover/tooltip probe as
the primary way to inspect deep samples — the tooltip only appears on a
`QEvent::ToolTip` pause and is cleared the moment the mouse leaves the image,
which makes it hard to discover and impossible to park on one pixel while
scrubbing frames. A node the user places and leaves connected is the
Nuke-familiar alternative.

Blocked on: needs codebase scouting before elaboration —
(a) how existing position/point knobs get a draggable viewer-overlay handle
(native-knob vs. OFX-plugin convention), so the picker reuses that mechanism
rather than inventing a new one, and
(b) precedent (if any) for a node's settings panel embedding a custom
non-knob widget, for the per-sample table.
Run `/cat-plan` to scout these and turn this into phases/tasks. See
`PLAN/DECISIONS/2026-09-18-deepsample-node-replaces-hover-probe.md`.

Acceptance sketch:
- A `DeepSample` node placed on a deep stream has a position settable either
  by typing X/Y or by dragging a Viewer overlay handle when the node is
  selected.
- Its settings panel shows a live table of every sample at that position for
  the current frame — raw/untidied, matching M18.P2.T3's precedent (tidying
  would show values the source file does not contain).
- The table updates on frame change and on position change without
  triggering a re-render of anything downstream (the node is pass-through).
- M18.P2.T3's `ViewerGL` hover/tooltip probe is removed; `DeepSample` is the
  only sample-inspection affordance.
