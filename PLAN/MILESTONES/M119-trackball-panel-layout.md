# M119 - Trackball Panel Layout

Follow-up to M44 - Trackball Colour Editing. At the default properties width the trackball wheel is small (its `sizeHint` is a fixed 160 px, `Gui/ColorWheelWidget.cpp:584`) and the Hue…Tint tracks are about 60 px, because the panel sits in the knob's below-row column between the label column and the right-hand button inset (`Gui/ColorAdjustPanel.cpp:326-378`). This milestone builds the layout candidates the user listed as independent, switchable axes in the real panel, ships one AppImage for the user to compare them on real nodes, then keeps only the winning combination and deletes the rest.

## Scope

**In:**
- A responsive wheel: square, at least today's 160 px, growing to fill the space the layout gives it, capped at about 320 px (DPI-scaled, tunable at UAT).
- Four independent dev axes, switchable live while `NATRON_COLOR_PANEL_DEV=1` is set:
  - **Left edge:** the panel starts at the field column (today) or extends into the label column.
  - **Right edge:** the panel's Hue…Tint lines stop at the button inset (today) or overflow into the column under the 4/swatch/▾/⊕ buttons.
  - **Slider style:** Natron `ScaleSliderQWidget` + `SpinBox` lines (today); the restored custom `ColorEditSlider` (absolute marker, relative drag elsewhere, chevrons) paired with Natron's real `SpinBox`; or vertical Natron sliders as columns to the right of the wheel.
  - **Placement:** in the knob (today) or a full-width trackball at the top of the properties bin.
- The bin-top placement: shown on demand, bound to one colour knob, retargeting as global mode does.
- Global mode starts in "C" on every fresh engage instead of remembering the last mode across invocations and nodes.
- An AppImage and test card for the user's pick, then a cleanup that makes the winner the only layout.

**Out:**
- The r/g/b/a channel lines move nowhere and keep their right inset on every axis; only the panel's Hue…Tint lines can overflow. (none)
- Keeping any losing axis as a user preference. (none)
- Changes to the colour maths, the gesture rates, global-mode keys or the icons. (M44 - Trackball Colour Editing shipped them; icon polish is M56 - Visual And Menu Polish)
- Simplified and viewer colour knobs, which keep the old popup. (none)

**Edge cases:**
- A properties bin narrower than the layout's minimum: the wheel never shrinks below 160 px; the sliders give way first, then the panel scrolls with the page as today.
- A wide or floating bin: the wheel stops at its cap; the extra width goes to the sliders (horizontal styles) or stays empty to the right (vertical style).
- Bin-top with no colour knob on the new top node: the bin-top panel hides.
- Bin-top while the bound knob's node is closed or deleted: the panel unbinds and hides; it holds no raw pointer to a dead `KnobGui`.
- Switching an axis while a gesture is running: the switch waits until the gesture ends, so a gesture is never split across two panel instances.
- Several colour knobs with ▾ open in the in-knob placement: each keeps its own panel, as today. In bin-top placement only one knob is bound at a time.

**Assumptions:**
- The custom slider is recoverable from history: `Gui/ColorEditSlider.{h,cpp}` and `Tests/ColorEditSlider_Test.cpp` as of `bed0a3207^`, before M44.P7.T7 deleted them.
- Extending into the label column needs its own page grid row spanning both columns of the page's `QGridLayout` (`Gui/KnobGuiContainerHelper.cpp:854-862` puts the label in column 0 and the field in column 1).

## Phase 119.1: Switchable layout spike

Every variant lives in the real panel. Tests go in `GuiTests`; the GUI evidence script is committed as `Tests/gui/m119_trackball_layout.py` and run with `Tests/gui/run-gui-test.sh Tests/gui/m119_trackball_layout.py build/fast/App/Natron` after a `fast` build (see the board's build-mode note). Agents do not build; the phase has one end build (T9).

- [ ] M119.P1.T1 — Add the dev layout switch: four axes, an env-var gate, a live change notifier and a dev bar on each panel
  - files: `Gui/ColorPanelLayoutDev.h` (new), `Gui/ColorPanelLayoutDev.cpp` (new), `Gui/ColorAdjustPanel.h`, `Gui/ColorAdjustPanel.cpp`, `Tests/ColorAdjustPanel_Test.cpp`
  - approach:
    - `struct ColorPanelLayout { bool extendLeft; bool extendRight; enum SliderStyle { Natron, Custom, Vertical } style; enum Placement { InKnob, BinTop } placement; }`, defaulting to today's layout (false, false, Natron, InKnob).
    - A process-wide singleton `QObject` holds the current value and emits `layoutChanged()`. `isDevEnabled()` reads `NATRON_COLOR_PANEL_DEV` once.
    - When enabled, each `ColorAdjustPanel` shows a one-line dev bar above its edit area: two checkboxes and two combos bound to the singleton. With the variable unset, the bar is never created.
    - The panel listens to `layoutChanged()` and rebuilds its edit area. A change that arrives during a gesture is deferred to `endGesture()`.
    - No `Engine/Settings` knobs: nothing about this is serialized.
  - verify: `GuiTests --gtest_filter=ColorAdjustPanel*`: with the variable unset there is no dev bar; with it set (via `qputenv` before construction) changing a combo emits `layoutChanged` once and a panel rebuilds; a change mid-gesture rebuilds only after `endGesture`. Existing cases stay green.
  - size: M

- [ ] M119.P1.T2 — Make the wheel square and responsive between 160 px and a ~320 px cap
  - files: `Gui/ColorWheelWidget.h`, `Gui/ColorWheelWidget.cpp`, `Gui/ColorAdjustPanel.cpp`, `Tests/ColorWheelWidget_Test.cpp`
  - approach:
    - `ColorWheelWidget` gets `hasHeightForWidth() = true`, `heightForWidth(w) = w`, an `Expanding` policy, `minimumSizeHint` 160 px and a `kWheelMaxPx` cap of 320 (both `TO_DPIX`). Painting already uses `min(width, height)`; centre the disc in any non-square rect.
    - The disc QImage cache is keyed on size, so a resize re-renders; rebuild it lazily on the next paint, not on every resize event during a drag of the bin splitter.
    - In the horizontal styles the panel gives the wheel the width left after the sliders' minimum (caption + spinbox + a 120 px track), clamped to [160, 320].
  - verify: `GuiTests --gtest_filter=ColorWheelWidget*`: a 600 px-wide panel gives a wheel of 320 px, a 300 px one gives at least 160 px, and the disc's drawn radius matches the widget's short side (±1 px). Existing paint and input cases stay green at 160 px.
  - size: M

- [ ] M119.P1.T3 — Add the left and right bound axes: a full-width page row for the panel, and the overflow inset
  - files: `Gui/KnobGui.h`, `Gui/KnobGuiContainerHelper.cpp`, `Gui/KnobGuiValue.h`, `Gui/KnobGuiValue.cpp`, `Gui/KnobGuiColor.cpp`
  - approach:
    - **Right.** When `extendRight` is on, `KnobGuiColor` feeds `ColorAdjustPanel::setRightInset(0)` instead of the buttons' width. The channel lines (`dimensionLines`) keep their inset whatever the axis says.
    - **Left.** Add a virtual `QWidget* KnobGui::getFullWidthRowWidget()` (default null). `KnobGuiContainerHelper`, after adding a knob's row, gives a non-null result its own grid row spanning both columns, directly below. `KnobGuiColor` returns a holder widget; when `extendLeft` is on the panel is reparented into the holder, otherwise back into the below-row column after the channel lines. Hiding and showing the knob hides and shows the holder too.
    - Reparenting runs inside the same update guard and bottom-up layout flush M44.P7.T8 uses for the ▾ toggle, so switching an axis does not flash.
  - verify: `GuiTests --gtest_filter=KnobGuiColor*` on a Grade `gain` panel at 500 px: with `extendLeft` the panel's left x equals the label column's left x (±1 px); with `extendRight` the Hue slider's right edge equals the ⊕ button's right edge (±1 px) while the "g" channel slider keeps today's right edge; a Blur `size` row is unchanged.
  - size: M

- [ ] M119.P1.T4 — Restore the custom slider and offer it as the Custom style, paired with Natron's SpinBox
  - files: `Gui/ColorEditSlider.h`, `Gui/ColorEditSlider.cpp`, `Tests/ColorEditSlider_Test.cpp`, `Tests/CMakeLists.txt`, `Gui/ColorAdjustPanel.cpp`
  - approach:
    - Restore the three files from `bed0a3207^` (`git show bed0a3207^:<path>`), and re-add the test to `Tests/CMakeLists.txt`.
    - Bring it up to date: gearing comes from `gear()` in `Engine/ColorWheelMath.h` read from each mouse event (as M44.P7.T1 did for `ScaleSliderQWidget`), ranges come from `sliderMapping(kind, role, rates)`, and the track gradient matches the Natron style's.
    - The panel's Custom style builds each line as caption, Natron `SpinBox`, `ColorEditSlider`. The slider's absolute marker drag and relative elsewhere drag map onto the existing gesture plumbing (one `beginGesture`/`endGesture` per drag, one undo step); the SpinBox keeps P7.T7's one-shot absolute edit.
  - verify: `GuiTests --gtest_filter=ColorEditSlider*:ColorAdjustPanel*`: a marker drag of 40 px moves exactly 40 px of range whatever the modifiers; a relative 80 px drag with Ctrl moves 8 px of range; in the Custom style a drag is one undo step and the SpinBox shows the new value after it.
  - size: L

- [ ] M119.P1.T5 — Give `ScaleSliderQWidget` a vertical orientation
  - files: `Gui/ScaleSliderQWidget.h`, `Gui/ScaleSliderQWidget.cpp`, `Tests/ScaleSliderQWidget_Test.cpp`
  - approach:
    - `setOrientation(Qt::Orientation)`, default horizontal. Vertical maps the long axis to y with the minimum at the bottom; mouse mapping, gearing accumulation, the marker and the optional track gradient all follow the long axis. Tick labels are omitted when vertical.
    - Horizontal paint and input stay byte-for-byte the same path.
  - verify: `GuiTests --gtest_filter=ScaleSliderQWidget*`: a 300 px vertical slider over [0, 1] gains about 0.1 for a 30 px upward drag and about 0.0333 for 100 px with Ctrl; a press at 25 % from the bottom jumps to 0.25; every existing horizontal case stays green.
  - size: M

- [ ] M119.P1.T6 — Build the Vertical style: slider columns to the right of the wheel
  - files: `Gui/ColorAdjustPanel.h`, `Gui/ColorAdjustPanel.cpp`, `Tests/ColorAdjustPanel_Test.cpp`
  - approach:
    - The edit area is the wheel, then one column per line (Hue, Sat, Value, Temp, Tint): caption on top, a vertical `ScaleSliderQWidget` (T5), the `SpinBox` underneath. Column heights follow the wheel's height.
    - The wheel takes the width the columns don't need, within T2's [160, 320]. `extendRight` widens the space available; any remainder stays empty to the right.
    - Signals and gesture plumbing are P7.T7's, unchanged; `getSlider`/`getSpinBox` keep returning the line's widgets.
  - verify: `GuiTests --gtest_filter=ColorAdjustPanel*` in Vertical style: five vertical sliders exist; a 1-dim knob shows only Value; each slider's height equals the wheel's (±2 px); an upward drag on Value raises value and is one undo step.
  - size: M

- [ ] M119.P1.T7 — Add the bin-top trackball panel, bound to one colour knob and retargeting on top-node change
  - files: `Gui/ColorTrackballBinPanel.h` (new), `Gui/ColorTrackballBinPanel.cpp` (new), `Gui/PropertiesBinWrapper.cpp`, `Gui/ColorTrackballGlobalMode.cpp`, `Tests/ColorTrackballBinPanel_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - One `ColorTrackballBinPanel` per properties bin, inserted at the top of the bin's layout, hidden by default. It holds a title (`Node.knob`), a close button and one `ColorAdjustPanel` driven through a `ColorKnobEditSession` on the bound knob, so gestures and undo behave as in-knob.
    - `bindTo(KnobGuiColor*)` / `unbind()`. It holds the knob GUI through a `QPointer`-style guard and unbinds when the knob GUI or its panel dies.
    - When the topmost node in the bin changes while it is open, it rebinds through global mode's existing target resolution (last-touched, else `gain` on Grade, master gain on ColorCorrect, else the first colour knob), and hides when the new top node has none.
    - All panel layout axes except Placement apply to it unchanged; Left/Right edges mean the bin's full width.
  - verify: `GuiTests --gtest_filter=ColorTrackballBinPanel*`: binding a Grade `gain` shows the panel with the right title and a Hue drag edits that knob in one undo step; raising a ColorCorrect to the top rebinds to master gain; raising a Blur hides it; deleting the bound node unbinds without a crash.
  - size: L

- [ ] M119.P1.T8 — Route ▾, Ctrl+E, Ctrl+Up/Down and the HUD to the bin-top panel in BinTop placement
  - files: `Gui/KnobGuiColor.h`, `Gui/KnobGuiColor.cpp`, `Gui/ColorTrackballGlobalMode.h`, `Gui/ColorTrackballGlobalMode.cpp`, `Tests/ColorTrackballGlobalMode_Test.cpp`
  - approach:
    - In BinTop placement the knob's ▾ becomes "push up": it unfolds the knob's r/g/b/a lines inline (as ▾ does today), binds the bin-top panel to this knob and shows it; ▾ again, or the panel's close button, hides it and re-folds per M44.P7.T8's rule. No inline adjust panel is shown. The tooltip says "Edit in the trackball panel at the top of the properties".
    - Ctrl+E engages global mode as today and also binds and shows the bin-top panel; Ctrl+Up/Down knob switching rebinds it. The HUD attaches inside the bin-top panel instead of under the knob.
    - In InKnob placement nothing changes.
  - verify: `GuiTests --gtest_filter=ColorTrackballGlobalMode*:KnobGuiColor*` in BinTop: ▾ on `gain` shows the bin panel bound to `gain` and unfolds r/g/b/a inline with no inline adjust panel; Ctrl+E then Ctrl+Down rebinds to `gamma`; the HUD's parent is the bin panel. InKnob cases stay green.
  - size: M

- [ ] M119.P1.T10 — Reset global mode to "C" on every fresh engage, so the mode no longer carries over between invocations or nodes
  - files: `Gui/ColorTrackballGlobalMode.cpp`, `Gui/ColorTrackballGlobalMode.h`, `Tests/ColorTrackballGlobalMode_Test.cpp`
  - approach:
    - `ColorTrackballGlobalMode` is one controller per `Gui` (`instanceFor`), so `_mode` outlives `exit()` and the next Ctrl+E on any node starts in whatever mode was last picked. In `engage()`, in the `if (!_engaged)` branch (a fresh invocation), set the mode back to `eColorGlobalModeC` through the same path `setMode` uses, so the HUD and readouts update; do it before `Q_EMIT engagedChanged(true)`.
    - An `engage()` that only moves an already-engaged mode onto another knob (Ctrl+Up/Down, BinTop retargeting from T7/T8) keeps the current mode: only leaving and re-engaging resets it. Update the doc comment on `engage` and `getMode` to say so.
    - No setting, no serialization: the default is always "C".
  - verify: `GuiTests --gtest_filter=ColorTrackballGlobalMode*`: engage, `setMode(eColorGlobalModeH)` (any non-C mode), `exit()`, engage on a different knob, and `getMode()` is `eColorGlobalModeC` with `stateChanged` emitted; engage, set a non-C mode, then engage on a second knob without exiting, and the mode is unchanged. Existing cases stay green.
  - size: S

- [ ] M119.P1.T9 — End build: run every suite, write the GUI evidence script, and capture each combination at two bin widths
  - files: `Tests/gui/m119_trackball_layout.py` (new)
  - approach:
    - One `fast` build and full ctest.
    - The script opens Grade and ColorCorrect with `NATRON_COLOR_PANEL_DEV=1`, and for each meaningful combination (both bound toggles × three styles × two placements) at the default bin width and at 2× captures a screenshot and records the wheel's diameter and the Hue track's length to `results.txt`.
    - It also checks no paint at a stale geometry on an axis switch (the M44.P7.T8 paint logger), that a drag on each style is one undo step, and that Ctrl+E on a second node after leaving the mode in a non-C mode shows "C" in the HUD (T10).
  - verify: ctest all green; `results.txt` has no FAIL and a wheel diameter and track length for every combination; the shots exist and are listed for the test card.
  - size: M

## Phase 119.2: UAT and pick

- [ ] M119.P2.T1 — Package the spike AppImage and write the test card
  - files: none in the tree (`build/appimages/`, the test card goes in the PM's message to the user)
  - approach: `package.sh` on the `fast` build. The test card says to launch with `NATRON_COLOR_PANEL_DEV=1`, lists the four axes and what each does, the measured wheel size and track length per combination at the default width, and the questions: which combination wins, and whether the 320 px cap and the 160 px floor are right. Share the key screenshots before the AppImage. Record the user's pick and any tuning in `## Decisions`, then re-check Phase 119.3's briefs against it (PLAN-FORMAT.md §5a).
  - verify: the AppImage launches with `--version`; the user's pick is recorded in `## Decisions`.
  - size: S

## Phase 119.3: Keep the winner

These briefs name every file a losing axis could leave behind; the PM narrows each one to the pick recorded at P2.T1.

- [ ] M119.P3.T1 — Make the picked combination the only layout and delete the dev switch and every losing path
  - files: `Gui/ColorPanelLayoutDev.{h,cpp}` (delete), `Gui/ColorAdjustPanel.{h,cpp}`, `Gui/KnobGuiColor.{h,cpp}`, `Gui/KnobGuiContainerHelper.cpp`, and whichever of `Gui/ColorEditSlider.*`, `Gui/ColorTrackballBinPanel.*`, `ScaleSliderQWidget`'s vertical path and `KnobGui::getFullWidthRowWidget` the pick does not use
  - approach: hard-code the picked values, remove the dev bar, the env var and the `layoutChanged` rebuild, and delete the unused widgets and hooks with their tests and `Tests/CMakeLists.txt` entries. `grep -rn 'NATRON_COLOR_PANEL_DEV\|ColorPanelLayout' Gui Tests` is empty afterwards. If this touches more than five non-deleted files, split it per widget.
  - verify: `GuiTests` for the kept widgets green; the removed tests are gone from the list; the grep is empty.
  - size: L

- [ ] M119.P3.T2 — End build, refresh the GUI evidence script to the final layout, and package the approval AppImage
  - files: `Tests/gui/m119_trackball_layout.py`
  - approach: reduce the script to the final layout at both widths, keep the size, undo and no-flash checks, one `fast` build and full ctest, `package.sh`, and share the screenshots before the AppImage.
  - verify: ctest all green; the script has no FAIL; the user approves the final layout on the AppImage.
  - size: M

**Verification gate:** full ctest green; `Tests/gui/m119_trackball_layout.py` reports no FAIL; at the default properties width the wheel is larger than 160 px; no `NATRON_COLOR_PANEL_DEV` or losing-variant code remains; the user approved the final layout on an AppImage.

## Decisions

- 2026-10-14 — **Global mode resets to "C" on a fresh engage (user):** the controller is per-`Gui`, so the last-used mode persisted across exits and nodes, which was confusing. Only a fresh invocation resets; retargeting an engaged mode keeps its mode. Task M119.P1.T10.
- 2026-10-13 — **Scope set by the user:** the trackball is too small; reconsider M44's "use Natron's normal float sliders" call. Candidates to try: sliders overflowing into the button column, the old custom sliders with a proper twiddle box, the wheel extending into the label column and into both label and button columns, vertical Hue…Tint sliders, and a full-width trackball at the top of the properties bin that reuses global mode's knob switching, with ▾ becoming a "push up" control.
- 2026-10-13 — **Tried in Natron, behind a dev switch:** the candidates are built in the real panel and compared on one AppImage, not in a mockup: real knobs and real bin widths are what decide it.
- 2026-10-13 — **Independent axes, not presets:** left edge, right edge, slider style and placement switch separately, so the best mix can be found.
- 2026-10-13 — **"Fix the twiddle box" means pairing the restored `ColorEditSlider` with Natron's real `SpinBox`** (user), so the numeric field behaves exactly like a float knob's.
- 2026-10-13 — **Only the panel's Hue…Tint lines overflow into the button column; the r/g/b/a channel lines stay where they are** (user).
- 2026-10-13 — **Bin-top placement (user):** shown on demand (▾ "push up" or Ctrl+E), retargets on top-node change through global mode's target resolution, and hosts the global-mode HUD; the r/g/b/a lines still unfold inline on the knob.
- 2026-10-13 — **Vertical style:** columns to the right of the wheel, caption on top and SpinBox underneath (user).
- 2026-10-13 — **Wheel size:** fills the space it is given, never below 160 px, capped at about 320 px, tunable at UAT (user).
- 2026-10-13 — **Losing variants are deleted** before merge; none becomes a preference (user).
- 2026-10-13 — **Deps M56, M69:** both are `doing` and edit `Gui/KnobGuiColor.cpp` and `Gui/KnobGuiValue.cpp`, which this milestone rewrites.
