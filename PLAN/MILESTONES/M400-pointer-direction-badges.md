# M400 - Pointer Direction Badges

Follow-on to M173 - Global Mode For Value Knobs. When global mode shows no mode graphic, the user still sees the direction badges (the `ColorTrackballPill`s such as "+Hue" / "−Sat", or a value kind's direction labels) ringed closely round the crosshair, with the HUD block under them. Today the "Crosshair only" feedback, and "Crosshair + reveal" between strokes, show only the following HUD block (`ColorTrackballGlobalMode::syncHudBlock`, `Gui/ColorTrackballGlobalMode.cpp` ~1265), while the badges live only inside the reveal window (`ColorTrackballReveal::setPillTexts`/`relayout`, `Gui/ColorTrackballReveal.cpp`), laid out outside the ring's outer radius.

Code facts (scouted on `main` d07d202ea, 2026-11-12):
- `colorGlobalModeFeedbackState` (`Gui/ColorTrackballGlobalMode.cpp` ~256) sets `hudBlockFollowsPointer = !graphicShown`: true always for crosshairOnly, and for crosshairReveal outside a stroke.
- Labels: `colorTrackballRevealDirectionLabels(ColorGlobalModeEnum)` (`Gui/ColorTrackballReveal.cpp` ~118; C returns empty strings) and `valueModeRevealDirectionLabels(GlobalModeKindEnum)` (`Gui/ValueModeDiscWidget.cpp` ~179). Targets push them into the reveal through `GlobalModeTarget::syncReveal` (`Gui/GlobalModeTarget.h`).
- The follow window is a `ColorTrackballHudBlock` made with `colorTrackballFollowCursorWindowFlags()` and placed by `followCursorTo` / `colorTrackballHudFollowPosition` (`Gui/ColorTrackballHud.{h,cpp}`); it is moved at ~1358 and ~1403 of the controller as the pointer moves.

## Scope

**In:**
- Direction badges round the pointer whenever the HUD block follows the pointer: always in "Crosshair only" (engaged and during strokes), and in "Crosshair + reveal" between strokes. Both modes keep the HUD block, now under the bottom badge.
- Badges sit close to the crosshair (a small gap round the cursor, not the ring radius) and follow the pointer, including during a stroke and across edge warps; the whole group stays on screen.
- Every target that has labels gets them: colour S, T and V, every value kind, and the transform group's active kind. Labels update live on a mode/kind switch (C/S/T/V, Q/W/E/R/T, Ctrl+Up/Down).

**Out:**
- C mode badges: C shows only the HUD block, as its HUD line already explains C (user). None.
- The "Widget cursor" feedback and the reveal window during a stroke in "Crosshair + reveal": unchanged. None.
- New label wording: the existing label functions are reused as they are. None.

**Edge cases:**
- A target whose labels are all empty (C) shows the HUD block alone, placed exactly as today.
- Near a screen edge the badge group and block are shifted together to stay on screen, as `colorTrackballRevealPosition` does for the reveal; the crosshair stays where the pointer is.
- Switching feedback in Preferences mid-engage applies through the existing `ColorGlobalModeFeedbackSwitch` path; the follow window shows or drops its badges on the next sync.
- Exit, modal-dialog exit and target loss hide the follow window, as they hide the block today.

**Assumptions:**
- Unconfirmed: verification is unit tests, the Xvfb GUI check and screenshots the user approves; no separate AppImage UAT, as M173's crosshair-only follow-up needed none.

## Phase 400.1: Badge labels and layout

- [ ] M400.P1.T1 — Expose each target's direction labels without a reveal
  - files: `Gui/GlobalModeTarget.h`, `Gui/ColorTrackballTarget.{h,cpp}`, `Gui/ValueKnobGlobalModeTarget.{h,cpp}`, `Gui/TransformGroupGlobalModeTarget.{h,cpp}`, `Tests/ColorTrackballTarget_Test.cpp`
  - approach: Add a pure virtual `ColorTrackballRevealLabels directionLabels() const` to `GlobalModeTarget`. Colour returns `colorTrackballRevealDirectionLabels(currentMode)`; a value knob returns `valueModeRevealDirectionLabels(kind)`; the transform group returns its active kind's labels. Make each target's `syncReveal` use the same function so the two can't drift.
  - verify: test cases asserting colour S gives "+Hue"/"−Hue"/"+Sat"/"−Sat", C gives four empty strings, and a Blur `size` value target and a Transform group in rotate give the same labels as `valueModeRevealDirectionLabels` for those kinds; existing `ColorTrackballTarget_Test` cases still pass.
  - size: M
- [ ] M400.P1.T2 — Pure layout for badges round a point
  - files: `Gui/ColorTrackballHud.{h,cpp}`, `Tests/ColorTrackballHud_Test.cpp`
  - approach: Add `colorTrackballPointerBadgeLayout(reach, pillSizes[4], blockSize, blockGap)` returning the four pill rects and the block rect relative to the pointer at the origin (right/left/top/bottom at `reach`, the block centred under the bottom pill; with every pill empty, the block alone placed as `colorTrackballHudFollowPosition` places it today), plus the united bounds. `reach` is a small DPI-scaled gap that clears the crosshair cursor; pick a constant in the file and name it. Do not change the reveal's own layout.
  - verify: `ColorTrackballHud_Test` cases: four pills don't overlap each other or a square of side 2×reach round the origin; the block sits under the bottom pill; with every pill empty the block lands where `colorTrackballHudFollowPosition` puts it.
  - size: M

## Phase 400.2: Pointer badge window

- [ ] M400.P2.T1 — Follow-pointer window with badges and HUD block
  - files: `Gui/ColorTrackballHud.{h,cpp}`, `Gui/ColorTrackballGlobalMode.{h,cpp}`, `Tests/ColorTrackballGlobalMode_Test.cpp`
  - approach: Replace the controller's standalone follow `ColorTrackballHudBlock` with a `ColorTrackballPointerHud` window (same `colorTrackballFollowCursorWindowFlags()`, never takes input or focus) holding four `ColorTrackballPill`s and a `ColorTrackballHudBlock` child, laid out by P1.T2 and masked to the pills and block (`colorTrackballRoundedRegion`), kept on screen as a group. `syncHudBlock` feeds it `target->directionLabels()` and `hudState()`; the existing pointer-move call sites (~1358, ~1403) move it with the pointer, in and out of strokes. Keep the object name `ColorTrackballHudWindow` and an accessor to the inner block so existing tests find it. Labels refresh on every `syncReveal`, so mode and kind switches update them.
  - verify: `ColorTrackballGlobalMode_Test` cases: in crosshairOnly the window is shown with four visible pills for an S-mode colour target, engaged and mid-stroke, and stays centred on the moved pointer; in C only the block is visible; in crosshairReveal the pills show between strokes and the window hides while the reveal is up; widgetCursor never shows it; existing tests pass.
  - size: L

## Phase 400.3: GUI check and screenshots

- [ ] M400.P3.T1 — Xvfb GUI script and screenshots
  - files: `build/m400-gui/` (script and fixtures, under `build/` per the Xvfb recipe)
  - approach: Set feedback to "Crosshair only", engage a Grade colour knob (S, T, V, then C), a Blur `size` knob and a Transform's rotate, with real key and mouse events; capture each with the pointer mid-screen and one near a screen corner; then "Crosshair + reveal" idle and mid-stroke. Check the pointer window's pill texts and visibility through the widget tree.
  - verify: zero FAIL; one PNG per state sent to the user with SendUserFile, and the user approves them.
  - size: M

**Verification gate:** release build plus both ctest legs green (including the new `ColorTrackballTarget_Test`, `ColorTrackballHud_Test` and `ColorTrackballGlobalMode_Test` cases); `build/m400-gui/` script zero FAIL under Xvfb; the user approves the screenshots.

## Decisions

- 2026-11-12 — Scope (user): C mode gets no badges; badges always follow the pointer, including during strokes; "Crosshair + reveal" shows them between strokes too, not only "Crosshair only".
