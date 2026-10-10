# M44 - Trackball Colour Editing

Full title: Trackball-style colour editing

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Add a "trackball" interaction to colour controls — holding modifier keys
and dragging adjusts hue/saturation/value/temperature directly on the
swatch, rather than only via sliders/dialogs.

Blocked on: not yet prioritized — captured as backlog (user request
2026-09-18); pairs naturally with M24's colour-panel work landing first.

Acceptance sketch:
- Dragging on a colour control with the documented modifier held adjusts
  hue, saturation, value, or temperature respectively.

## Decisions

- 2026-10-10 — **Scope set by the user:** trackball-style grading on a colour-wheel widget (angle = hue, radius = saturation, adjusted together by an x/y drag, like a DaVinci/Pablo trackball, similar to Nuke's wheel), plus individual hue, saturation and value gestures, a temperature adjustment, and gearing (a modifier for slower/faster drags). Every non-value control preserves value. The wheel lives in a Nuke-style dropdown adjustment panel on any node's colour knobs; the node-colour button is out of scope. The maths runs on linear values, HDR-preserving, keeping hue, saturation and brightness separate; perceptual spaces other than HSV are to be investigated. The user asked for design pitches before elaboration (`PLAN/DESIGN/2026-10-10-trackball-colour-editing.md`).
- 2026-10-10 — **Deps `-` (user-confirmed):** M44 touches only the colour-knob GUI, which M24 does not, so it runs alongside M24.
