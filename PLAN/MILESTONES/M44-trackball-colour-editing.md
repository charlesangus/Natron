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
- 2026-10-10 — **Colour model, temperature and popup (user):** the luma–chroma split (Y from the working space's luma weights, read from OCIO, plus a 2D chroma plane; hue and saturation edits leave Y exact), physically based Kelvin/tint temperature for gain, colour and gamma knobs with straight axes for offsets, and the old colour popup replaced by a "Values" tab in the new panel. The pitch choice (A/B/C) is still open.
- 2026-10-10 — **Hybrid design (user, after trying the A/B/C mockups, claude.ai artifact `2Umo1q3iMH5v4i41YYtcUe`):**
  - Gearing as in pitch A: Ctrl ×0.1, Shift ×4, Ctrl+Shift ×0.01.
  - Dedicated drag zones as in A, but one per function, all horizontal and side by side. The slider's marker shows the absolute value: dragging the marker positions absolutely, and dragging anywhere else on the slider adjusts relatively, as in A.
  - A "global trackball" mode. Once it is engaged, H, S, V, T or C picks the property, and a drag anywhere on screen adjusts it, with A's gearing. Esc cancels. Ctrl+Up/Down or Ctrl+scroll moves to the next or previous colour knob on the current node (the top node in the properties bin). Axes:
    - T: left/right is cool/warm, up/down is magenta/green.
    - C: left/right is hue, up/down is saturation.
    - H or S: up/down is value.
  - Alpha tracks luma.
  - No rim auto-zoom. The puck may go past the wheel's edge, where it parks on the edge with a changed look to show it is off the wheel, and the scroll wheel zooms the trackball.
  - Over the trackball itself, holding H, S, V or T while dragging adjusts just that parameter.
- 2026-10-10 — **Hybrid clarifications (user):**
  - "Alpha tracks luma" means a value edit scales alpha by the same factor as luma (for Offset, it adds the same amount), so a gain of (2,2,2,1) becomes (2,2,2,2). Hue, sat, temp and tint edits leave alpha alone, and alpha keeps its own slider.
  - Global-mode axes: H is hue on x and value on y; S is saturation on x and value on y; V changes value with either axis.
  - Global mode is engaged by a button on each colour knob row, or a shortcut (G with the cursor over a colour knob or its panel). It shows a HUD, Esc cancels, and Enter or a click commits.
  - Next step: a single hybrid Qt mockup for the user to try before elaboration.
