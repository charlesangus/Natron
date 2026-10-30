# M275 - Viewer Widget Editing

Make edits performed through Viewer overlays behave exactly like edits made through node properties: they set keys when auto-keying/keyed values require it, enter the node's undo stack, and do not snap back after a drag.

## Scope

**In:**
- Transform and other native Viewer widgets commit their final values through the same knob-edit path as properties panels.
- A drag at a keyed/auto-keyed time creates or updates the keyframe and is one node undo step.
- Overlay state refreshes from the committed knob value without overwriting the edit.

**Out:**
- Roto/RotoPaint-specific gesture and editor work, owned by M224 - Fluid Roto And RotoPaint Interaction and M261 - Roto Keyframes In Dope Sheet And Curve Editor.

## Phase 275.1: Shared overlay edit path

- [ ] M275.P1.T1 — Route Viewer widget drags through knob editing
  - files: native overlay interaction base, transform overlay, knob edit/undo bridge, overlay tests
  - approach: Start one knob edit transaction on press, update the preview during motion, and commit through the node's normal knob path on release so keying, notifications and undo match property edits.
  - verify: A Transform widget drag changes the knob, does not snap back, and one undo restores the prior value.
  - size: L

- [ ] M275.P1.T2 — Apply keyframe policy to overlay edits
  - files: overlay edit transaction, animation/keyframe helpers, GUI tests
  - approach: Reuse the properties-panel decision for existing animation and auto-key; create/update a key at the current frame without changing unkeyed dimensions.
  - verify: Dragging at several frames creates the same keys and interpolation as editing the values directly.
  - size: M

## Phase 275.2: Regression coverage

- [ ] M275.P2.T1 — Cover representative Viewer widgets and node undo
  - files: new Viewer-widget GUI test, Transform and representative overlay fixtures
  - approach: Exercise move, scale and rotate on Transform plus one other native widget, checking node undo/redo, key indicators and save/reopen.
  - verify: The GUI script passes under Xvfb and its screenshots show the overlay at the keyed values.
  - size: M

**Verification gate:** full build and ctest pass; the Viewer-widget GUI test passes under Xvfb; direct and overlay edits produce equivalent keys and undo history; a release AppImage is accepted by the user.
