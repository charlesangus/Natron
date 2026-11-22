# M511 - Viewer Toolbar Fits 1920x1080

At 1920x1080, scale 1, with the default layout full-screen, the viewer's first toolbar row (`Gui/ViewerTab.cpp:466-751`) is wider than the default viewer pane. The row holds the layer, alpha, channels, zoom, sync, centre, clip, full-frame, RoI, render scale, Auto Proxy, refresh and pause controls, then A, operator and B. As a result the B label is overlapped and the B combo is clipped. Every control must be fully visible at the minimum supported screen.

## Scope

**In:**
- Measuring the row against the default pane.
- Making it fit: shrinkable combos with elided text and the full text in the tooltip, tighter spacers, and moving controls only if the user approves.
- A GUI script that asserts the fit.
- Screenshots for sign-off.

**Out:**
- The second row (gain, gamma and so on), unless P1 shows it also overflows; file that separately.
- Viewer panes the user has split narrower than the default.

**Edge cases:**
- A long layer or input name: the combo elides it and the tooltip shows it in full.
- Wider screens: the row is unchanged apart from any spacing change.

**Assumptions:**
- `Tests/gui/guitest.py` (`HOST_WINDOW_SIZE`, default 1920x1080) gives the PR #125 harness geometry.
- On `main` nothing follows the B combo except a stretch (`Gui/ViewerTab.cpp:749-751`), so the clipping is the row's own width; P1.T1 measures it.

## Phase 511.1: Measure and decide

- [ ] M511.P1.T1 — GUI script measuring the first viewer row in the default layout
  - files: `Tests/gui/m511_viewer_toolbar_fit.py` (new; model on `Tests/gui/m253_render_settings_layout.py`, `Tests/gui/guitest.py`)
  - approach: start in the default layout, full-screen at 1920x1080. Find the ViewerTab's first-row widgets and print each one's geometry, `sizeHint` and `minimumSizeHint`, plus the row and pane widths. Assert every widget lies inside the row and none overlaps its neighbour. Save one screenshot of the viewer toolbar. The assertion is expected to fail before the fix.
  - verify: PM runs it under Xvfb on the phase build and records the per-widget widths and the overflow as a decision
  - size: M
- [ ] M511.P1.T2 — Choose how the row fits
  - files: read-only: `Gui/ViewerTab.cpp`, the P1.T1 numbers
  - approach: consultant (opus) works out how many pixels shrinkable combos (`AdjustToMinimumContentsLengthWithIcon` with a `minimumContentsLength`) and tighter spacers would save. If that is not enough, it sets out the options for the PM to put to the user with trade-offs: moving A/operator/B to the second row, or folding rarely used toggles into a menu.
  - verify: a decision recorded; any control move is approved by the user
  - size: L

## Phase 511.2: Fix

- [ ] M511.P2.T1 — Make the first row fit the default pane
  - files: `Gui/ViewerTab.cpp` (and `Gui/ViewerTabPrivate.h` if a widget moves)
  - approach: apply the P1.T2 decision, with elided combo text and the full name in the tooltip. Labels and tooltips keep their current wording, and no control is removed.
  - verify: `Tests/gui/m511_viewer_toolbar_fit.py` passes under Xvfb on the phase build
  - size: M

**Verification gate:** `m511_viewer_toolbar_fit.py` green under Xvfb at 1920x1080; one screenshot each of the default layout and a long layer name is shared with the user and approved before sign-off; full debug PR CI green.
