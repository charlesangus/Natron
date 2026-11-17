# M451 - Viewer Stale Cue Experiments

Bring back a viewer cue for "the image on screen is out of date", in a form that doesn't flicker or distract. M302 - Viewer Render State Cues drew an amber outline around the whole viewer after a 150 ms delay. In F6 - Unified Tiered Cache UAT (2026-11-15) the user found it flickery and too distracting, so F6 removed the outline and its delay preference before landing. The engine side stayed on main: `ViewerInstance::staleChanged(bool)` / `isStale()` (`Engine/ViewerInstance.h`), covered by `Tests/ViewerStaleState_Test.cpp`. The removed GUI code is in M302's merge, `65889b6cd` on `feature/f6-unified-tiered-cache` (`Gui/ViewerGL.cpp` `paintGL` border quads, `Gui/ViewerTab10.cpp` `onViewerStaleChanged` / `refreshStaleBorder`, `Engine/Settings.cpp` `viewerStaleBorderDelayMs`). Use it as reference only.

The milestone has three steps. First it separates **when** the cue shows (a debounce with a show delay and a minimum on-time) from **what** it draws (the visual style). Then it builds five styles behind a temporary preference and hands the user screenshots and an AppImage. Once the user picks a style, it deletes the other styles.

## Scope

**In:**
- A pure timing class: show delay, minimum on-time, suppressed during playback. Covered by GuiTests.
- Five styles, chosen by a temporary `staleCueStyle` preference: a debounced full outline, a subtle outline, a corner dot, a line on one side, and a busy spinner in the viewer toolbar (user, 2026-11-16).
- Delay and hold preferences.
- An Xvfb GUI script that screenshots each style while stale.
- UAT, then removal of the styles the user didn't pick.

**Out:**
- Changes to the engine's stale state or `ViewerStaleState_Test`. The engine signal is already right; only the GUI cue was the problem.
- The node-graph device dots and lit edges from M302. They shipped and are not part of this milestone.
- Animated cues (pulsing, fading). Not requested. Raise one during UAT if a static style isn't enough.

**Edge cases:**
- Stale for less than the show delay: nothing is drawn.
- The cue is visible and the image becomes current before the minimum on-time has passed: the cue stays up until the on-time ends, then clears. If the image goes stale again before then, the cue stays up with no gap.
- Playback starts: the cue clears at once and the timers stop. When playback stops and the viewer is still stale, the show delay starts again from zero.
- Several viewers: each `ViewerTab` has its own timing state.
- Style set to Off: nothing is drawn and no timers run.

**Assumptions:**
- `ViewerTab::onEngineStarted` / `onEngineStopped` (`Gui/ViewerTab10.cpp`) are still the right playback hooks, as they were for M302.

## Phase 451.1: Timing and preferences

- [ ] M451.P1.T1 — Add the stale-cue preferences
  - files: Engine/Settings.cpp, Engine/Settings.h
  - approach: In `Settings::initializeKnobsViewers`, next to the checkerboard knobs, add three knobs. Each label is the readable form of its script name. (1) `staleCueStyle` ("Stale Cue Style"), a KnobChoice with entries Off, Outline, Subtle Outline, Corner Dot, Edge Line, Toolbar Spinner; default Corner Dot. (2) `staleCueDelay` ("Stale Cue Delay"), a KnobInt in ms, min 0, default 400. (3) `staleCueHold` ("Stale Cue Hold"), a KnobInt in ms, min 0, default 500. Tooltips say what each controls and that nothing is drawn during playback. Add getters `getStaleCueStyle()` (an enum `StaleCueStyleEnum` in Settings.h), `getStaleCueDelayMs()` and `getStaleCueHoldMs()`, and set the defaults in `setDefaultValues`. Mirror how `_checkerboardTileSize` is declared, created and read.
  - verify: the debug build compiles; the existing Settings/preferences ctest suite passes.
  - size: M
- [ ] M451.P1.T2 — Add a pure stale-cue timing class with GuiTests
  - files: Gui/ViewerStaleCueTiming.h, Gui/ViewerStaleCueTiming.cpp, Gui/CMakeLists.txt, Tests/ViewerStaleCueTiming_Test.cpp, Tests/CMakeLists.txt (GuiTests_SOURCES)
  - approach: A class with no Qt widgets and no timers. Its inputs are `setStale(bool, nowMs)`, `setPlayback(bool, nowMs)` and `advance(nowMs)`, plus delay and hold set at construction or through setters. It exposes `visible()` and `nextDeadlineMs()` (the time the caller should next call `advance`, or none). Rules: (1) visible once stale has lasted the delay with no playback; (2) once shown, it stays visible until stale is false *and* hold ms have passed since it was shown; (3) playback forces it hidden and resets both clocks. Edge cases are listed in this file's Scope. The caller drives it with a single-shot QTimer, so the logic can be tested without one.
  - verify: a new GuiTests gtest covers every Scope edge case: short stale is never visible; a fast stale→current→stale toggle inside the hold leaves no gap; playback hides it and resets the delay; delay 0 shows it at once; hold 0 clears it at once.
  - size: M

## Phase 451.2: Styles

- [ ] M451.P2.T1 — Drive the cue from ViewerTab through the timing class
  - files: Gui/ViewerTab.cpp, Gui/ViewerTab.h, Gui/ViewerTab10.cpp, Gui/ViewerTabPrivate.h, Gui/ViewerTabPrivate.cpp, Gui/ViewerGL.h, Gui/ViewerGL.cpp, Gui/ViewerGLPrivate.h
  - approach: Connect `ViewerInstance::staleChanged` to a `ViewerTab` slot, as M302 did (see `65889b6cd` ViewerTab.cpp / ViewerTab10.cpp). Own a `ViewerStaleCueTiming` and one single-shot QTimer that fires at `nextDeadlineMs()`. Feed playback in from `onEngineStarted` / `onEngineStopped`. Read delay and hold from Settings each time the cue arms, so changing a preference takes effect without a restart. For the GL styles, push the result to `ViewerGL::setStaleCue(StaleCueStyleEnum style, bool visible)`, which stores both and calls `update()` only when either changed. For Toolbar Spinner, call a `ViewerTab::setStaleSpinnerVisible(bool)` stub that T3 fills in, and pass `visible = false` to ViewerGL. Style Off never arms the timer. Draw nothing yet; T2 and T3 add the drawing.
  - verify: compiles; GuiTests and ViewerStaleState_Test pass.
  - size: M
- [ ] M451.P2.T2 — Draw the four styles in ViewerGL::paintGL
  - files: Gui/ViewerGL.cpp, Gui/ViewerGLPrivate.h
  - approach: Draw in `paintGL` after the selection rectangle, under `GLProtectAttrib`, the same way M302's border did (`65889b6cd` ViewerGL.cpp). Size everything in screen pixels, converted through `glShadow` as M302 did. **Outline**: amber (1, 0.7, 0) quads on all four sides, 2 px, the M302 look with the new timing. **Subtle Outline**: 1 px on all four sides, amber at 40% alpha with blending on. **Corner Dot**: an amber filled circle (a GL_TRIANGLE_FAN), 8 px across, inset 10 px from the viewport's top-right corner. **Edge Line**: a single 3 px amber bar along the top edge only. Draw nothing for Off.
  - verify: compiles; the P3 GUI script screenshots each style.
  - size: M
- [ ] M451.P2.T3 — Add the Toolbar Spinner style
  - files: Gui/ViewerTab.cpp, Gui/ViewerTab10.cpp, Gui/ViewerTabPrivate.h, Gui/ViewerTabPrivate.cpp
  - approach: Add a small busy-spinner widget to the viewer's top toolbar, at its right end. Size it to the toolbar's icon size and keep its slot reserved when hidden (fixed size, or `QSizePolicy::setRetainSizeWhenHidden`) so the toolbar never reflows. Animate it with a QTimer only while it is shown: a rotating arc painted in amber in `paintEvent`, about 12 frames per second. `setStaleSpinnerVisible(bool)` shows or hides it and starts or stops the timer. Its tooltip says the viewer image is out of date. Nothing changes when another style is selected.
  - verify: compiles; GuiTests pass; the P3 GUI script screenshots it; the toolbar width is the same with the spinner shown and hidden.
  - size: M

## Phase 451.3: Screenshots and UAT

- [ ] M451.P3.T1 — Add an Xvfb GUI script that screenshots each style and checks the timing
  - files: Tests/gui/m451_stale_cue.py
  - approach: Follow the harness pattern of `Tests/gui/m302_device_dots.py`, with the window at 1920x1080. Use M302's removed `Tests/gui/m302_stale_border.py` (in `65889b6cd`) for how it held a render long enough to see the viewer stale. For each of Outline, Subtle Outline, Corner Dot, Edge Line and Toolbar Spinner: set `staleCueStyle`, make the viewer stale past the delay, then save one screenshot per style under the script's output dir. Also check (a) that a render shorter than the delay never shows the cue and (b) that the cue clears within delay + hold + slack after the image becomes current. Read visibility through a test-only accessor on ViewerGL, or on the spinner widget's `isVisible()`, not from pixels.
  - verify: the script passes in the remote GUI gate (`build.sh --from-run <id> --gui`) and produces five screenshots.
  - size: M
- [ ] M451.P3.T2 — UAT: user picks a style and timing
  - files: none (AppImage + steps .md, screenshots via SendUserFile)
  - approach: Send the five screenshots one file each. Package a release AppImage with a matching steps .md: try each style, try several delay and hold values on a slow graph and a fast one, and say which style and timings to keep. Set `Needs: uat`. Record the outcome in `## Decisions`: the chosen style, its default delay and hold, and whether `staleCueStyle` stays as a user preference (with Off) or is removed. If no style is acceptable, put the user's direction in `## Decisions` and add tasks for it before P4.
  - verify: the user's choice is recorded in `## Decisions`.
  - size: S

## Phase 451.4: Keep the chosen style

- [ ] M451.P4.T1 — Remove the styles the user didn't pick
  - files: Gui/ViewerGL.cpp, Gui/ViewerGL.h, Gui/ViewerTab.cpp, Gui/ViewerTab10.cpp, Gui/ViewerTabPrivate.h, Engine/Settings.cpp, Engine/Settings.h, Tests/gui/m451_stale_cue.py
  - approach: Per P3.T2's decision, delete the drawing code (or the toolbar spinner widget), choice entries and enum values of the styles that weren't picked. If the decision drops the style preference, remove `staleCueStyle` and its getter and draw the kept style unconditionally. Otherwise keep it with only Off plus the kept style. Set the delay and hold defaults to the chosen values. Trim the GUI script to the kept style and keep its timing checks.
  - verify: compiles; GuiTests pass; the trimmed GUI script passes remotely; a grep finds no removed enum value or knob name left behind.
  - size: M

## Decisions

- 2026-11-16 — A toolbar spinner is one of the styles (user): added to the drafted outline, subtle outline, corner dot and edge line styles.

**Verification gate:** full ctest passes, including the GuiTests timing gtest and ViewerStaleState_Test. `Tests/gui/m451_stale_cue.py` passes in the remote GUI gate. The user has accepted the kept style on an AppImage (P3.T2) and confirmed the final build.
- 2026-11-17 — P1.T1-P3.T1 code in 586cac225 (lane .worktrees/m451); gate running. Deviations: StaleCueStyleEnum sits at namespace scope with an int base so ViewerGL.h can forward-declare it; ViewerGL exposes natronViewerStale for the GUI script; the Corner Dot's edge is 10 px from both edges.
