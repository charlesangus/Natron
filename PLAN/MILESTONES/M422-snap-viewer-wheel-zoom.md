# M422 - Snap Viewer Wheel Zoom

Wheel zoom in the 2D viewer steps along the zoom menu's presets (10, 25, 50, 75, 100, 125, 150, 200, 400, 800, 1600, 2400, 3200, 6400%) instead of multiplying by a continuous factor, so it lands on 100%, 200%, 50% rather than 112% or 87%. The viewer's Zoom In and Zoom Out shortcuts and menu actions step along the same ladder.

**Today:** `ViewerGL::wheelEvent` (`Gui/ViewerGL.cpp` ~:2743) scales by `NATRON_WHEEL_ZOOM_PER_DELTA ^ angleDelta` (×1.2 per notch, `Gui/GuiDefines.h`), and shows `(int)(100 * factor)`, which truncates. `ViewerTab::zoomIn`/`zoomOut` (`Gui/ViewerTab40.cpp` ~:1363) multiply by 1.1 and 0.9. The global Zoom In/Out keys in `ViewerTab::keyPressEvent` (`Gui/ViewerTab10.cpp` ~:861) send a synthetic 120-delta wheel event, so they follow the wheel. The presets are added as literal strings in `Gui/ViewerTab.cpp` ~:207.

## Scope

**In:**
- One preset ladder, shared by the zoom menu and the stepping code.
- Wheel zoom steps one preset per notch and keeps the image point under the cursor fixed.
- `ViewerTab::zoomIn`/`zoomOut` (viewer shortcut group and the menu's `+`/`-` actions) step one preset, about the viewer centre as today.
- Beyond the ends, the ladder continues by halving below 10% (5, 2.5, 1.25%…) and doubling above 6400% (12800%…), within the zoom context's existing min/max.
- The zoom menu shows the exact preset text after a step (`25%`, never `24%`).

**Out:**
- Node graph, curve editor, histogram, timeline and 3D viewer wheel zoom — unchanged.
- Adding new entries to the zoom menu (user chose the existing presets).
- Fit, the 100% shortcut and picking a menu entry — unchanged; they already land where asked.
- A preference to turn snapping off.

**Edge cases:**
- Off-ladder level (after Fit, or a typed value, e.g. 87%): the first step goes to the next preset in the step's direction (100% in, 75% out), never to the nearest one behind it.
- On a preset within floating-point noise (relative difference under 0.5%): treated as on it, so a step moves a full preset.
- High-resolution wheels and trackpads (deltas of 2–28): deltas accumulate; each full 120 steps one preset, and a direction change resets the accumulator. Several notches in one event step several presets.
- At the zoom context's min or max: a further step does nothing, with no jitter.
- Synced viewers: still follow via `synchronizeOtherViewersProjection` after each step.
- Horizontal wheel and Ctrl-wheel frame seeking: unchanged.

**Assumptions:**
- `ZoomContext::zoom(centerX, centerY, scale)` with `scale = target / current` lands exactly on `target` when it is within min/max (no aspect-ratio side effects for the viewer's uniform zoom).

## Phase 422.1: Preset Ladder

- [ ] M422.P1.T1 — Shared zoom preset ladder with stepping function
  - files: new `Gui/ViewerZoomLadder.h` (and `.cpp` if needed), `Gui/ViewerTab.cpp`, `Gui/CMakeLists.txt` or the Gui sources list, new `Tests/ViewerZoomLadder_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: Define the preset list once (percent values, current order) and a pure function `double stepZoomPreset(double currentFactor, int steps, double minFactor, double maxFactor)` that returns the factor `steps` presets away, applying the off-ladder, 0.5% tolerance, halving/doubling-beyond-the-ends and clamp rules from Scope. Populate the zoom combobox from the list instead of literal strings, keeping the 100% entry as `level100Action` at the same index (the 100% shortcut sets index 7).
  - verify: gtest covers 100%→125%→150%→200%→400% and back; 87% +1 → 100%, 87% −1 → 75%; 10% −1 → 5%; 6400% +1 → 12800%; clamp at min/max returns the bound; 99.8% treated as 100%; multi-step jumps. Zoom menu entries unchanged in a Xvfb launch or the existing viewer GUI script.
  - size: M

## Phase 422.2: Snapping Steps

- [ ] M422.P2.T1 — Wheel zoom steps along the ladder
  - files: `Gui/ViewerGL.cpp`, `Gui/ViewerGLPrivate.h`, new `Tests/gui/m422_viewer_zoom_snap.py`
  - approach: In `ViewerGL::wheelEvent`, accumulate `angleDelta().y()` in a new private member (reset on direction change), and for each full 120 call `stepZoomPreset`; apply `zoomCtx.zoom(cursorPoint, target / current)` so the cursor point stays fixed. Keep the mipmap-level and cache-line logic, `zoomOrPannedSinceLastFit`, viewer sync and the render check. Emit `zoomChanged` with the rounded percent (`std::lround`). Model the GUI script on `Tests/gui/m338_viewer_input_keys.py`, sending real `QWheelEvent`s to the viewer's GL widget.
  - verify: GUI script: from Fit, wheel-in notches show 100%, 125%, 150%, 200%, 400% in the zoom menu (from the next preset above the fit level), wheel-out returns along the same values; 30-delta events need four to step once; the image coordinate under the cursor is the same before and after a step (read through the viewer's Python or zoom API). Fails on current main.
  - size: M
- [ ] M422.P2.T2 — Zoom In and Zoom Out actions step along the ladder
  - files: `Gui/ViewerTab40.cpp`, `Tests/gui/m422_viewer_zoom_snap.py`
  - approach: Replace the ×1.1/×0.9 in `ViewerTab::zoomIn`/`zoomOut` with `stepZoomPreset(current, ±1, …)` and set the combobox text from the rounded percent. Bounds come from the same zoom context the wheel uses.
  - verify: GUI script extension: from 87%, the viewer Zoom In shortcut gives 100%, then 125%; Zoom Out from 100% gives 75%; the menu's `+`/`-` actions do the same; the global Zoom In key (synthetic wheel) still steps one preset.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`, including `ViewerZoomLadder` and `m422_viewer_zoom_snap.py` under Xvfb. On a UAT AppImage (with its steps `.md`), wheel and keyboard zoom in the viewer land only on menu presets, with a mouse wheel and a trackpad, and pivot on the cursor; the user approves.

## Decisions

- 2026-11-13 — Wheel steps along the existing menu presets, continuing by halving/doubling past 10% and 6400% (user): no new menu entries.
- 2026-11-13 — Viewer Zoom In/Zoom Out shortcuts and menu actions snap to the same ladder (user).
- 2026-11-13 — Off-ladder levels step to the next preset in the wheel's direction, not the nearest (cat-plan): a notch then always zooms the way the user turned the wheel.
