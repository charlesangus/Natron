# M119 - Trackball Panel Layout

Follow-up to M44 - Trackball Colour Editing. At the default properties width the trackball wheel is small (its `sizeHint` is a fixed 160 px, `Gui/ColorWheelWidget.cpp:584`) and the Hue…Tint tracks are about 60 px, because the panel sits in the knob's below-row column between the label column and the right-hand button inset (`Gui/ColorAdjustPanel.cpp:326-378`). This milestone builds the layout candidates the user listed as independent, switchable axes in the real panel, ships one AppImage for the user to compare them on real nodes, then keeps only the winning combination and deletes the rest. It also tries cursor and HUD designs for global mode (crosshair cursor, a HUD that follows the cursor, a press-to-reveal mode graphic) on the same switch.

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
- Two more dev axes for global mode's feedback, on the same switch:
  - **Feedback:** today (HUD under the knob or in the bin-top panel, arrow cursor); **A** crosshair cursor with the HUD following it; **C** a mode graphic revealed at the press point; **A+C** both; **W** the mode widget replaces the cursor on engage, follows the pointer, and freezes in place on press.
  - **Puck:** how the reveal graphic's marker behaves, **relative** (starts at the disc's centre, travels with the stroke) or **absolute** (sits where the knob's current value falls on the gradient). Only matters with C, A+C or W.
- The reveal graphic: C mode shows the real `ColorWheelWidget`; S, T and V show discs of the wheel's diameter with the same outer ring, filled with a gradient that says what the stroke does. S: hue shifts left and right, white at the bottom to highly saturated at the top. T: warm to cool left to right, green to magenta bottom to top. V: white at the top right, black at the bottom left.
- An AppImage and test card for the user's pick, then a cleanup that makes the winner the only layout.

**Out:**
- The r/g/b/a channel lines move nowhere and keep their right inset on every axis; only the panel's Hue…Tint lines can overflow. (none)
- Keeping any losing axis as a user preference. (none)
- Changes to the colour maths, the gesture rates, global-mode keys or the icons, and to what the HUD's readout says (only where it is drawn changes). (M44 - Trackball Colour Editing shipped them; icon polish is M56 - Visual And Menu Polish)
- Simplified and viewer colour knobs, which keep the old popup. (none)
- Any other cursor designs, such as a per-mode glyph cursor or a cursor that becomes the trackball: pitched and not picked. (none)

**Edge cases:**
- A properties bin narrower than the layout's minimum: the wheel never shrinks below 160 px; the sliders give way first, then the panel scrolls with the page as today.
- A wide or floating bin: the wheel stops at its cap; the extra width goes to the sliders (horizontal styles) or stays empty to the right (vertical style).
- Bin-top with no colour knob on the new top node: the bin-top panel hides.
- Bin-top while the bound knob's node is closed or deleted: the panel unbinds and hides; it holds no raw pointer to a dead `KnobGui`.
- Switching an axis while a gesture is running: the switch waits until the gesture ends, so a gesture is never split across two panel instances.
- Several colour knobs with ▾ open in the in-knob placement: each keeps its own panel, as today. In bin-top placement only one knob is bound at a time.
- Feedback A or C with the HUD in BinTop placement: Feedback wins, the HUD is not attached to the bin-top panel while a non-today Feedback value is set.
- Press-to-reveal near a screen edge: the graphic is clamped to the screen; the puck's start is still the press point's disc centre (relative) so nothing jumps.
- The following HUD near a screen edge flips to the other side of the cursor; it never takes mouse events.
- The cursor is a crosshair from engage to exit and returns on exit or when a modal dialog takes over (as global mode already leaves for one). While a reveal graphic is up the cursor is hidden and comes back at release.
- WidgetCursor (W): from engage the mode widget (T14's window) is centred on the pointer and tracks it, the real cursor is hidden; on press it stops where it is and the stroke moves the puck inside it as in Reveal; on release it resumes following. A mode switch swaps the widget under the pointer, and exit hides it and restores the cursor. The following HUD runs alongside it, as in Crosshair.
- Absolute puck on S, T and V: only the axes with an absolute meaning move the puck off centre (S vertical saturation; T temp and tint; V along the diagonal). S's hue-shift axis has no absolute value, so the puck stays centred horizontally.
- Reveal graphic with a gesture that ends by a lost grab or app deactivation: it hides exactly as the stroke ends.
- A Feedback or Puck change arrives mid-stroke: deferred to the stroke's end, as every other axis.

**Assumptions:**
- The custom slider is recoverable from history: `Gui/ColorEditSlider.{h,cpp}` and `Tests/ColorEditSlider_Test.cpp` as of `bed0a3207^`, before M44.P7.T7 deleted them.
- Unconfirmed: the reveal graphic is 240 px (DPI-scaled) and tunable at UAT.
- Unconfirmed: the following HUD is a frameless click-through top-level tool window, offset about 16 px right and below the cursor; a platform that refuses such a window is not covered.
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

- [ ] M119.P1.T11 — Add the Feedback and Puck dev axes, and the crosshair cursor
  - files: `Gui/ColorPanelLayoutDev.h`, `Gui/ColorPanelLayoutDev.cpp`, `Gui/ColorTrackballGlobalMode.h`, `Gui/ColorTrackballGlobalMode.cpp`, `Tests/ColorTrackballGlobalMode_Test.cpp`
  - approach:
    - Extend T1's `ColorPanelLayout` with `enum Feedback { Today, Crosshair, Reveal, CrosshairReveal, WidgetCursor } feedback` and `enum Puck { Relative, Absolute } puck`, defaulting to Today and Relative. T1's dev bar gains the two combos (Puck only enabled with Reveal, CrosshairReveal or WidgetCursor); both stay inert without `NATRON_COLOR_PANEL_DEV`.
    - With Crosshair or CrosshairReveal, `engage()` pushes `QApplication::setOverrideCursor(Qt::CrossCursor)` on a fresh engage, and `exit()` and `leaveForModalWidget()` pop it, so every exit path restores the cursor exactly once.
    - A Feedback or Puck change mid-stroke is deferred to `endStroke()`, like the other axes.
  - verify: `GuiTests --gtest_filter=ColorTrackballGlobalMode*`: with Crosshair, engage makes `QApplication::overrideCursor()` a cross, exit and a leave for a modal widget restore it, and two engage/exit cycles leave no override cursor behind; with Today the cursor is never touched.
  - size: M

- [ ] M119.P1.T12 — Make the HUD follow the cursor in the Crosshair and CrosshairReveal feedback
  - files: `Gui/ColorTrackballHud.h`, `Gui/ColorTrackballHud.cpp`, `Gui/ColorTrackballGlobalMode.cpp`, `Gui/KnobGuiColor.cpp`, `Tests/ColorTrackballHud_Test.cpp`
  - approach:
    - Add `ColorTrackballHud::setFollowCursor(bool)`: when on, the HUD is a frameless, click-through top-level tool window (`Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowTransparentForInput`, no activation) positioned from the global-mode controller on every pointer move at an offset of about (16, 16) DPI-scaled to the cursor's below and right. When that would leave the screen holding the cursor, it flips to the other side on that axis.
    - The controller skips `attachGlobalModeHud` while the feedback is not Today (InKnob and BinTop alike) and hides the HUD at exit.
    - The HUD's content, legend and readout are unchanged.
  - verify: `GuiTests --gtest_filter=ColorTrackballHud*:ColorTrackballGlobalMode*`: with Crosshair the HUD's window flags are as listed and a move to (100, 100) puts it at the offset; a move within 10 px of the screen's right and bottom edges puts it left of and above the cursor; with Today the HUD is attached under the knob exactly as before.
  - size: M

- [ ] M119.P1.T13 — Add the S, T and V gradient discs for the reveal graphic
  - files: `Gui/ColorModeDiscWidget.h` (new), `Gui/ColorModeDiscWidget.cpp` (new), `Tests/ColorModeDiscWidget_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - One painted widget taking a mode (S, T or V) and a puck position in [-1, 1]². It draws a disc the size of its rect with the outer ring `ColorWheelWidget` draws (reuse its ring helper, or factor it out to a shared function if it is a private method), then the gradient inside it: S hue left to right with white at the bottom to full saturation at the top; T warm to cool left to right and green to magenta bottom to top; V white at the top right to black at the bottom left. Gradient colours come from `Engine/ColorWheelMath.h` so they match the knob's maths. Cache the gradient image per size.
    - The puck is a small ring marker clamped to the rim.
  - verify: `GuiTests --gtest_filter=ColorModeDiscWidget*`: rendering each mode at 240 px and sampling the image, the S disc is whiter at the bottom than the top and its left and right samples have different hues; the T disc is warmer at the left than the right and greener at the bottom than the top; the V disc is lighter at the top right than the bottom left; a puck position of (2, 2) is drawn clamped on the rim.
  - size: M

- [ ] M119.P1.T14 — Show the press-to-reveal graphic for the Reveal and CrosshairReveal feedback
  - files: `Gui/ColorTrackballReveal.h` (new), `Gui/ColorTrackballReveal.cpp` (new), `Gui/ColorTrackballGlobalMode.h`, `Gui/ColorTrackballGlobalMode.cpp`, `Tests/ColorTrackballReveal_Test.cpp` (new)
  - approach:
    - `ColorTrackballReveal` is a transparent, click-through top-level window (same flags as T12) holding a `ColorWheelWidget` in C mode and a `ColorModeDiscWidget` (T13) in S, T and V, 240 px DPI-scaled, centred on the press point and clamped to the screen.
    - On `beginStroke()` the controller shows it and hides the cursor (`QApplication::setOverrideCursor(Qt::BlankCursor)`, popped in `endStroke()`); `endStroke()` and `endStrokeWithoutRelease()` hide it, and a mode change mid-stroke swaps the widget.
    - **Relative puck:** starts at the centre and moves with the stroke's accumulated delta (scaled so a full-rate stroke crosses the radius at about the gesture's normal range), clamped to the rim. **Absolute puck:** placed from the knob's current `KnobColorValue` on the axes that have an absolute meaning (C: the wheel's own marker; S: vertical saturation; T: temp and tint; V: along the diagonal) and moved from there.
    - The panel's own wheel and the HUD are unaffected.
  - verify: `GuiTests --gtest_filter=ColorTrackballReveal*`: a press at (500, 400) shows a window centred there; with Relative the puck is at the centre on press and moves with a synthetic stroke; with Absolute it starts at the value's position; release and a lost grab hide the window and pop the blank cursor, leaving no override cursor beyond the crosshair; a press at a screen corner keeps the window inside the screen; the stroke's value changes are identical with and without the reveal.
  - size: L

- [ ] M119.P1.T15 — Add the WidgetCursor feedback: the mode widget replaces the cursor from engage and freezes on press
  - files: `Gui/ColorTrackballReveal.h`, `Gui/ColorTrackballReveal.cpp`, `Gui/ColorTrackballGlobalMode.h`, `Gui/ColorTrackballGlobalMode.cpp`, `Tests/ColorTrackballReveal_Test.cpp`
  - approach:
    - Reuses T14's `ColorTrackballReveal` window with a follow mode: on a fresh engage with WidgetCursor, the controller shows it centred on the pointer (blank override cursor pushed, popped on every exit path as in T11), and on every pointer move outside a stroke re-centres it on the pointer, clamped to the screen.
    - `beginStroke()` freezes the window at its current position instead of re-centring on the press (the reveal path); `endStroke()`/`endStrokeWithoutRelease()` end the stroke and resume following, without hiding the window. The Puck setting applies as in T14.
    - Mode changes swap the widget in place; the following HUD (T12) is shown as in Crosshair. Today, Crosshair, Reveal and CrosshairReveal behave exactly as before.
  - verify: `GuiTests --gtest_filter=ColorTrackballReveal*:ColorTrackballGlobalMode*`: with WidgetCursor, engage shows the window centred on the pointer and the override cursor is blank; a synthetic move re-centres it; a press freezes it and moves to the puck; release resumes following; setMode swaps the widget; exit hides it and leaves no override cursor.
  - size: M

- [ ] M119.P1.T9 — End build: run every suite, write the GUI evidence script, and capture each combination at two bin widths
  - files: `Tests/gui/m119_trackball_layout.py` (new)
  - approach:
    - One `fast` build and full ctest.
    - The script opens Grade and ColorCorrect with `NATRON_COLOR_PANEL_DEV=1`, and for each meaningful layout combination (both bound toggles × three styles × two placements) at the default bin width and at 2× captures a screenshot and records the wheel's diameter and the Hue track's length to `results.txt`.
    - It also checks no paint at a stale geometry on an axis switch (the M44.P7.T8 paint logger), that a drag on each style is one undo step, and that Ctrl+E on a second node after leaving the mode in a non-C mode shows "C" in the HUD (T10). For the Feedback axis it also engages global mode with each of Today, Crosshair, Reveal, CrosshairReveal and WidgetCursor, takes one screenshot per mode (C, S, T, V) of the reveal graphic with each Puck setting, and records the override cursor shape before, during and after a stroke.
  - verify: ctest all green; `results.txt` has no FAIL and a wheel diameter and track length for every combination; the shots exist and are listed for the test card, including the four reveal graphics under each Puck setting.
  - size: M

## Phase 119.2: UAT and pick

- [ ] M119.P2.T1 — Package the spike AppImage and write the test card
  - files: none in the tree (`build/appimages/`, the test card goes in the PM's message to the user)
  - approach: `package.sh` on the `fast` build. The test card says to launch with `NATRON_COLOR_PANEL_DEV=1`, lists the six axes (the four layout axes plus Feedback and Puck) and what each does, the measured wheel size and track length per combination at the default width, and the questions: which combination wins, which Feedback value and Puck behaviour win (and whether the graphic's 240 px and the HUD's offset are right), and whether the 320 px cap and the 160 px floor are right. Share the key screenshots before the AppImage. Record the user's pick and any tuning in `## Decisions`, then re-check Phase 119.3's briefs against it (PLAN-FORMAT.md §5a).
  - verify: the AppImage launches with `--version`; the user's pick is recorded in `## Decisions`.
  - size: S

## Phase 119.3: Keep the winner

These briefs name every file a losing axis could leave behind; the PM narrows each one to the pick recorded at P2.T1.

- [ ] M119.P3.T1 — Make the picked combination the only layout and delete the dev switch and every losing path
  - files: `Gui/ColorPanelLayoutDev.{h,cpp}` (delete), `Gui/ColorAdjustPanel.{h,cpp}`, `Gui/KnobGuiColor.{h,cpp}`, `Gui/KnobGuiContainerHelper.cpp`, and whichever of `Gui/ColorEditSlider.*`, `Gui/ColorTrackballBinPanel.*`, `ScaleSliderQWidget`'s vertical path, `KnobGui::getFullWidthRowWidget`, `Gui/ColorTrackballReveal.*`, `Gui/ColorModeDiscWidget.*` and the HUD's follow-cursor mode the pick does not use
  - approach: hard-code the picked values, remove the dev bar, the env var and the `layoutChanged` rebuild, the Feedback and Puck switches (keeping only the picked cursor, HUD and reveal behaviour), and delete the unused widgets and hooks with their tests and `Tests/CMakeLists.txt` entries. `grep -rn 'NATRON_COLOR_PANEL_DEV\|ColorPanelLayout' Gui Tests` is empty afterwards. If this touches more than five non-deleted files, split it per widget.
  - verify: `GuiTests` for the kept widgets green; the removed tests are gone from the list; the grep is empty.
  - size: L

- [ ] M119.P3.T2 — End build, refresh the GUI evidence script to the final layout, and package the approval AppImage
  - files: `Tests/gui/m119_trackball_layout.py`
  - approach: reduce the script to the final layout at both widths, keep the size, undo and no-flash checks, one `fast` build and full ctest, `package.sh`, and share the screenshots before the AppImage.
  - verify: ctest all green; the script has no FAIL; the user approves the final layout on the AppImage.
  - size: M

**Verification gate:** full ctest green; the global-mode feedback the user picked (cursor, HUD placement, reveal graphic and puck) works on the AppImage; `Tests/gui/m119_trackball_layout.py` reports no FAIL; at the default properties width the wheel is larger than 160 px; no `NATRON_COLOR_PANEL_DEV` or losing-variant code remains; the user approved the final layout on an AppImage.

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
- 2026-10-24 — **Cursor and HUD designs go on the same switch (user):** a `Feedback` axis (Today, A crosshair + following HUD, C press-to-reveal graphic, A+C) and a `Puck` axis, tried on the one AppImage and deleted with the losers. Tasks M119.P1.T11 to T14. The per-mode glyph cursor and the HUD-with-inline-graphic designs were pitched and not picked.
- 2026-10-24 — **Reveal graphic (user):** C mode shows the current trackball widget at the click; S, T and V show discs with the wheel's ring, filled with the gradients listed under Scope, and the user wants both a relative puck (from the centre) and an absolute puck (at the current value) to try.
- 2026-10-24 — **Real cursor is hidden during a reveal stroke (user, on recommendation):** the puck is the feedback.
- 2026-10-24 — **The following HUD is a frameless click-through top-level window that flips at screen edges (user, on recommendation).**
- 2026-10-24 — **WidgetCursor feedback added (user):** the mode widget replaces the cursor on entering global mode, follows the pointer, and freezes in place on press. Task M119.P1.T15, which depends on T14.
- 2026-10-24 — **Claimed with M138 in new lanes 3 and 4 (user).** Freshness check at claim: anchors (`ColorWheelWidget.cpp:584`, `bed0a3207`) still hold; deps M56, M69 are done.
- 2026-10-24 — **T10 and T3 have no unit test:** `engage()` needs a real `Gui`, and GuiTests has no harness that builds a Grade properties panel. Their checks (re-engage shows "C"; left/right edges at 500 px) go into T9's Xvfb script.
- 2026-10-24 — **T13 duplicates the wheel's ring code** in `ColorModeDiscWidget.cpp` because `ColorWheelWidget` was being edited in parallel; T14 makes both share one ring function.
- 2026-10-24 — Code so far: T10 `ebcc66784`, T5 `308e64a35`, T13 `0751c682e`, T1–T3 `e73dfd841`; checked off after T9's build.
- 2026-10-24 — **Host reboot mid-run:** every Phase 119.1 task except T9 is committed (e73dfd841, 308e64a35, 511cb32f0, 518afca81, b6b46833b, ebcc66784, 49aaeef37, 0751c682e, c323d41e5, 02a287bd2, 6cc3ab7c0, a44785f42). Remote fast ctest on a44785f42 is green (run 40067778845); T9's evidence script is re-run on that AppImage before the boxes are checked.
