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
- **After the UAT pick (2026-10-24), superseding the axes above:** the panel spans the label column to the row's last button, with custom sliders (whole-slider relative drag) paired with Natron's SpinBox, in the knob. User preferences: global-mode feedback (Crosshair + reveal, Widget cursor) and track size (Regular; Large: a wheel up to 400 px with r/g/b/a and Hue…Tint stacked to its right). Right-click exits global mode, as Esc does, without opening a context menu. The S/T/V reveal discs show the drag's result (see P3.T9).

**Out:**
- In Regular the r/g/b/a lines stay above the panel; they move only in Large. (none)
- Keeping any losing axis as a user preference; only Feedback and track size are preferences. (none)
- Changes to the colour maths, the gesture rates, global-mode keys or the icons, and to what the HUD's readout says (only where it is drawn changes). (M44 - Trackball Colour Editing shipped them; icon polish is M56 - Visual And Menu Polish)
- Simplified and viewer colour knobs, which keep the old popup. (none)
- Any other cursor designs, such as a per-mode glyph cursor or a cursor that becomes the trackball: pitched and not picked. (none)

**Edge cases:**
- Large in a bin too narrow for wheel + right column: the Regular arrangement is used.
- Right-click during a left-button stroke: ignored; the stroke continues.
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

- [x] M119.P1.T1 — Add the dev layout switch: four axes, an env-var gate, a live change notifier and a dev bar on each panel
  - files: `Gui/ColorPanelLayoutDev.h` (new), `Gui/ColorPanelLayoutDev.cpp` (new), `Gui/ColorAdjustPanel.h`, `Gui/ColorAdjustPanel.cpp`, `Tests/ColorAdjustPanel_Test.cpp`
  - approach:
    - `struct ColorPanelLayout { bool extendLeft; bool extendRight; enum SliderStyle { Natron, Custom, Vertical } style; enum Placement { InKnob, BinTop } placement; }`, defaulting to today's layout (false, false, Natron, InKnob).
    - A process-wide singleton `QObject` holds the current value and emits `layoutChanged()`. `isDevEnabled()` reads `NATRON_COLOR_PANEL_DEV` once.
    - When enabled, each `ColorAdjustPanel` shows a one-line dev bar above its edit area: two checkboxes and two combos bound to the singleton. With the variable unset, the bar is never created.
    - The panel listens to `layoutChanged()` and rebuilds its edit area. A change that arrives during a gesture is deferred to `endGesture()`.
    - No `Engine/Settings` knobs: nothing about this is serialized.
  - verify: `GuiTests --gtest_filter=ColorAdjustPanel*`: with the variable unset there is no dev bar; with it set (via `qputenv` before construction) changing a combo emits `layoutChanged` once and a panel rebuilds; a change mid-gesture rebuilds only after `endGesture`. Existing cases stay green.
  - size: M

- [x] M119.P1.T2 — Make the wheel square and responsive between 160 px and a ~320 px cap
  - files: `Gui/ColorWheelWidget.h`, `Gui/ColorWheelWidget.cpp`, `Gui/ColorAdjustPanel.cpp`, `Tests/ColorWheelWidget_Test.cpp`
  - approach:
    - `ColorWheelWidget` gets `hasHeightForWidth() = true`, `heightForWidth(w) = w`, an `Expanding` policy, `minimumSizeHint` 160 px and a `kWheelMaxPx` cap of 320 (both `TO_DPIX`). Painting already uses `min(width, height)`; centre the disc in any non-square rect.
    - The disc QImage cache is keyed on size, so a resize re-renders; rebuild it lazily on the next paint, not on every resize event during a drag of the bin splitter.
    - In the horizontal styles the panel gives the wheel the width left after the sliders' minimum (caption + spinbox + a 120 px track), clamped to [160, 320].
  - verify: `GuiTests --gtest_filter=ColorWheelWidget*`: a 600 px-wide panel gives a wheel of 320 px, a 300 px one gives at least 160 px, and the disc's drawn radius matches the widget's short side (±1 px). Existing paint and input cases stay green at 160 px.
  - size: M

- [x] M119.P1.T3 — Add the left and right bound axes: a full-width page row for the panel, and the overflow inset
  - files: `Gui/KnobGui.h`, `Gui/KnobGuiContainerHelper.cpp`, `Gui/KnobGuiValue.h`, `Gui/KnobGuiValue.cpp`, `Gui/KnobGuiColor.cpp`
  - approach:
    - **Right.** When `extendRight` is on, `KnobGuiColor` feeds `ColorAdjustPanel::setRightInset(0)` instead of the buttons' width. The channel lines (`dimensionLines`) keep their inset whatever the axis says.
    - **Left.** Add a virtual `QWidget* KnobGui::getFullWidthRowWidget()` (default null). `KnobGuiContainerHelper`, after adding a knob's row, gives a non-null result its own grid row spanning both columns, directly below. `KnobGuiColor` returns a holder widget; when `extendLeft` is on the panel is reparented into the holder, otherwise back into the below-row column after the channel lines. Hiding and showing the knob hides and shows the holder too.
    - Reparenting runs inside the same update guard and bottom-up layout flush M44.P7.T8 uses for the ▾ toggle, so switching an axis does not flash.
  - verify: `GuiTests --gtest_filter=KnobGuiColor*` on a Grade `gain` panel at 500 px: with `extendLeft` the panel's left x equals the label column's left x (±1 px); with `extendRight` the Hue slider's right edge equals the ⊕ button's right edge (±1 px) while the "g" channel slider keeps today's right edge; a Blur `size` row is unchanged.
  - size: M

- [x] M119.P1.T4 — Restore the custom slider and offer it as the Custom style, paired with Natron's SpinBox
  - files: `Gui/ColorEditSlider.h`, `Gui/ColorEditSlider.cpp`, `Tests/ColorEditSlider_Test.cpp`, `Tests/CMakeLists.txt`, `Gui/ColorAdjustPanel.cpp`
  - approach:
    - Restore the three files from `bed0a3207^` (`git show bed0a3207^:<path>`), and re-add the test to `Tests/CMakeLists.txt`.
    - Bring it up to date: gearing comes from `gear()` in `Engine/ColorWheelMath.h` read from each mouse event (as M44.P7.T1 did for `ScaleSliderQWidget`), ranges come from `sliderMapping(kind, role, rates)`, and the track gradient matches the Natron style's.
    - The panel's Custom style builds each line as caption, Natron `SpinBox`, `ColorEditSlider`. The slider's absolute marker drag and relative elsewhere drag map onto the existing gesture plumbing (one `beginGesture`/`endGesture` per drag, one undo step); the SpinBox keeps P7.T7's one-shot absolute edit.
  - verify: `GuiTests --gtest_filter=ColorEditSlider*:ColorAdjustPanel*`: a marker drag of 40 px moves exactly 40 px of range whatever the modifiers; a relative 80 px drag with Ctrl moves 8 px of range; in the Custom style a drag is one undo step and the SpinBox shows the new value after it.
  - size: L

- [x] M119.P1.T5 — Give `ScaleSliderQWidget` a vertical orientation
  - files: `Gui/ScaleSliderQWidget.h`, `Gui/ScaleSliderQWidget.cpp`, `Tests/ScaleSliderQWidget_Test.cpp`
  - approach:
    - `setOrientation(Qt::Orientation)`, default horizontal. Vertical maps the long axis to y with the minimum at the bottom; mouse mapping, gearing accumulation, the marker and the optional track gradient all follow the long axis. Tick labels are omitted when vertical.
    - Horizontal paint and input stay byte-for-byte the same path.
  - verify: `GuiTests --gtest_filter=ScaleSliderQWidget*`: a 300 px vertical slider over [0, 1] gains about 0.1 for a 30 px upward drag and about 0.0333 for 100 px with Ctrl; a press at 25 % from the bottom jumps to 0.25; every existing horizontal case stays green.
  - size: M

- [x] M119.P1.T6 — Build the Vertical style: slider columns to the right of the wheel
  - files: `Gui/ColorAdjustPanel.h`, `Gui/ColorAdjustPanel.cpp`, `Tests/ColorAdjustPanel_Test.cpp`
  - approach:
    - The edit area is the wheel, then one column per line (Hue, Sat, Value, Temp, Tint): caption on top, a vertical `ScaleSliderQWidget` (T5), the `SpinBox` underneath. Column heights follow the wheel's height.
    - The wheel takes the width the columns don't need, within T2's [160, 320]. `extendRight` widens the space available; any remainder stays empty to the right.
    - Signals and gesture plumbing are P7.T7's, unchanged; `getSlider`/`getSpinBox` keep returning the line's widgets.
  - verify: `GuiTests --gtest_filter=ColorAdjustPanel*` in Vertical style: five vertical sliders exist; a 1-dim knob shows only Value; each slider's height equals the wheel's (±2 px); an upward drag on Value raises value and is one undo step.
  - size: M

- [x] M119.P1.T7 — Add the bin-top trackball panel, bound to one colour knob and retargeting on top-node change
  - files: `Gui/ColorTrackballBinPanel.h` (new), `Gui/ColorTrackballBinPanel.cpp` (new), `Gui/PropertiesBinWrapper.cpp`, `Gui/ColorTrackballGlobalMode.cpp`, `Tests/ColorTrackballBinPanel_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - One `ColorTrackballBinPanel` per properties bin, inserted at the top of the bin's layout, hidden by default. It holds a title (`Node.knob`), a close button and one `ColorAdjustPanel` driven through a `ColorKnobEditSession` on the bound knob, so gestures and undo behave as in-knob.
    - `bindTo(KnobGuiColor*)` / `unbind()`. It holds the knob GUI through a `QPointer`-style guard and unbinds when the knob GUI or its panel dies.
    - When the topmost node in the bin changes while it is open, it rebinds through global mode's existing target resolution (last-touched, else `gain` on Grade, master gain on ColorCorrect, else the first colour knob), and hides when the new top node has none.
    - All panel layout axes except Placement apply to it unchanged; Left/Right edges mean the bin's full width.
  - verify: `GuiTests --gtest_filter=ColorTrackballBinPanel*`: binding a Grade `gain` shows the panel with the right title and a Hue drag edits that knob in one undo step; raising a ColorCorrect to the top rebinds to master gain; raising a Blur hides it; deleting the bound node unbinds without a crash.
  - size: L

- [x] M119.P1.T8 — Route ▾, Ctrl+E, Ctrl+Up/Down and the HUD to the bin-top panel in BinTop placement
  - files: `Gui/KnobGuiColor.h`, `Gui/KnobGuiColor.cpp`, `Gui/ColorTrackballGlobalMode.h`, `Gui/ColorTrackballGlobalMode.cpp`, `Tests/ColorTrackballGlobalMode_Test.cpp`
  - approach:
    - In BinTop placement the knob's ▾ becomes "push up": it unfolds the knob's r/g/b/a lines inline (as ▾ does today), binds the bin-top panel to this knob and shows it; ▾ again, or the panel's close button, hides it and re-folds per M44.P7.T8's rule. No inline adjust panel is shown. The tooltip says "Edit in the trackball panel at the top of the properties".
    - Ctrl+E engages global mode as today and also binds and shows the bin-top panel; Ctrl+Up/Down knob switching rebinds it. The HUD attaches inside the bin-top panel instead of under the knob.
    - In InKnob placement nothing changes.
  - verify: `GuiTests --gtest_filter=ColorTrackballGlobalMode*:KnobGuiColor*` in BinTop: ▾ on `gain` shows the bin panel bound to `gain` and unfolds r/g/b/a inline with no inline adjust panel; Ctrl+E then Ctrl+Down rebinds to `gamma`; the HUD's parent is the bin panel. InKnob cases stay green.
  - size: M

- [x] M119.P1.T10 — Reset global mode to "C" on every fresh engage, so the mode no longer carries over between invocations or nodes
  - files: `Gui/ColorTrackballGlobalMode.cpp`, `Gui/ColorTrackballGlobalMode.h`, `Tests/ColorTrackballGlobalMode_Test.cpp`
  - approach:
    - `ColorTrackballGlobalMode` is one controller per `Gui` (`instanceFor`), so `_mode` outlives `exit()` and the next Ctrl+E on any node starts in whatever mode was last picked. In `engage()`, in the `if (!_engaged)` branch (a fresh invocation), set the mode back to `eColorGlobalModeC` through the same path `setMode` uses, so the HUD and readouts update; do it before `Q_EMIT engagedChanged(true)`.
    - An `engage()` that only moves an already-engaged mode onto another knob (Ctrl+Up/Down, BinTop retargeting from T7/T8) keeps the current mode: only leaving and re-engaging resets it. Update the doc comment on `engage` and `getMode` to say so.
    - No setting, no serialization: the default is always "C".
  - verify: `GuiTests --gtest_filter=ColorTrackballGlobalMode*`: engage, `setMode(eColorGlobalModeH)` (any non-C mode), `exit()`, engage on a different knob, and `getMode()` is `eColorGlobalModeC` with `stateChanged` emitted; engage, set a non-C mode, then engage on a second knob without exiting, and the mode is unchanged. Existing cases stay green.
  - size: S

- [x] M119.P1.T11 — Add the Feedback and Puck dev axes, and the crosshair cursor
  - files: `Gui/ColorPanelLayoutDev.h`, `Gui/ColorPanelLayoutDev.cpp`, `Gui/ColorTrackballGlobalMode.h`, `Gui/ColorTrackballGlobalMode.cpp`, `Tests/ColorTrackballGlobalMode_Test.cpp`
  - approach:
    - Extend T1's `ColorPanelLayout` with `enum Feedback { Today, Crosshair, Reveal, CrosshairReveal, WidgetCursor } feedback` and `enum Puck { Relative, Absolute } puck`, defaulting to Today and Relative. T1's dev bar gains the two combos (Puck only enabled with Reveal, CrosshairReveal or WidgetCursor); both stay inert without `NATRON_COLOR_PANEL_DEV`.
    - With Crosshair or CrosshairReveal, `engage()` pushes `QApplication::setOverrideCursor(Qt::CrossCursor)` on a fresh engage, and `exit()` and `leaveForModalWidget()` pop it, so every exit path restores the cursor exactly once.
    - A Feedback or Puck change mid-stroke is deferred to `endStroke()`, like the other axes.
  - verify: `GuiTests --gtest_filter=ColorTrackballGlobalMode*`: with Crosshair, engage makes `QApplication::overrideCursor()` a cross, exit and a leave for a modal widget restore it, and two engage/exit cycles leave no override cursor behind; with Today the cursor is never touched.
  - size: M

- [x] M119.P1.T12 — Make the HUD follow the cursor in the Crosshair and CrosshairReveal feedback
  - files: `Gui/ColorTrackballHud.h`, `Gui/ColorTrackballHud.cpp`, `Gui/ColorTrackballGlobalMode.cpp`, `Gui/KnobGuiColor.cpp`, `Tests/ColorTrackballHud_Test.cpp`
  - approach:
    - Add `ColorTrackballHud::setFollowCursor(bool)`: when on, the HUD is a frameless, click-through top-level tool window (`Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowTransparentForInput`, no activation) positioned from the global-mode controller on every pointer move at an offset of about (16, 16) DPI-scaled to the cursor's below and right. When that would leave the screen holding the cursor, it flips to the other side on that axis.
    - The controller skips `attachGlobalModeHud` while the feedback is not Today (InKnob and BinTop alike) and hides the HUD at exit.
    - The HUD's content, legend and readout are unchanged.
  - verify: `GuiTests --gtest_filter=ColorTrackballHud*:ColorTrackballGlobalMode*`: with Crosshair the HUD's window flags are as listed and a move to (100, 100) puts it at the offset; a move within 10 px of the screen's right and bottom edges puts it left of and above the cursor; with Today the HUD is attached under the knob exactly as before.
  - size: M

- [x] M119.P1.T13 — Add the S, T and V gradient discs for the reveal graphic
  - files: `Gui/ColorModeDiscWidget.h` (new), `Gui/ColorModeDiscWidget.cpp` (new), `Tests/ColorModeDiscWidget_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - One painted widget taking a mode (S, T or V) and a puck position in [-1, 1]². It draws a disc the size of its rect with the outer ring `ColorWheelWidget` draws (reuse its ring helper, or factor it out to a shared function if it is a private method), then the gradient inside it: S hue left to right with white at the bottom to full saturation at the top; T warm to cool left to right and green to magenta bottom to top; V white at the top right to black at the bottom left. Gradient colours come from `Engine/ColorWheelMath.h` so they match the knob's maths. Cache the gradient image per size.
    - The puck is a small ring marker clamped to the rim.
  - verify: `GuiTests --gtest_filter=ColorModeDiscWidget*`: rendering each mode at 240 px and sampling the image, the S disc is whiter at the bottom than the top and its left and right samples have different hues; the T disc is warmer at the left than the right and greener at the bottom than the top; the V disc is lighter at the top right than the bottom left; a puck position of (2, 2) is drawn clamped on the rim.
  - size: M

- [x] M119.P1.T14 — Show the press-to-reveal graphic for the Reveal and CrosshairReveal feedback
  - files: `Gui/ColorTrackballReveal.h` (new), `Gui/ColorTrackballReveal.cpp` (new), `Gui/ColorTrackballGlobalMode.h`, `Gui/ColorTrackballGlobalMode.cpp`, `Tests/ColorTrackballReveal_Test.cpp` (new)
  - approach:
    - `ColorTrackballReveal` is a transparent, click-through top-level window (same flags as T12) holding a `ColorWheelWidget` in C mode and a `ColorModeDiscWidget` (T13) in S, T and V, 240 px DPI-scaled, centred on the press point and clamped to the screen.
    - On `beginStroke()` the controller shows it and hides the cursor (`QApplication::setOverrideCursor(Qt::BlankCursor)`, popped in `endStroke()`); `endStroke()` and `endStrokeWithoutRelease()` hide it, and a mode change mid-stroke swaps the widget.
    - **Relative puck:** starts at the centre and moves with the stroke's accumulated delta (scaled so a full-rate stroke crosses the radius at about the gesture's normal range), clamped to the rim. **Absolute puck:** placed from the knob's current `KnobColorValue` on the axes that have an absolute meaning (C: the wheel's own marker; S: vertical saturation; T: temp and tint; V: along the diagonal) and moved from there.
    - The panel's own wheel and the HUD are unaffected.
  - verify: `GuiTests --gtest_filter=ColorTrackballReveal*`: a press at (500, 400) shows a window centred there; with Relative the puck is at the centre on press and moves with a synthetic stroke; with Absolute it starts at the value's position; release and a lost grab hide the window and pop the blank cursor, leaving no override cursor beyond the crosshair; a press at a screen corner keeps the window inside the screen; the stroke's value changes are identical with and without the reveal.
  - size: L

- [x] M119.P1.T15 — Add the WidgetCursor feedback: the mode widget replaces the cursor from engage and freezes on press
  - files: `Gui/ColorTrackballReveal.h`, `Gui/ColorTrackballReveal.cpp`, `Gui/ColorTrackballGlobalMode.h`, `Gui/ColorTrackballGlobalMode.cpp`, `Tests/ColorTrackballReveal_Test.cpp`
  - approach:
    - Reuses T14's `ColorTrackballReveal` window with a follow mode: on a fresh engage with WidgetCursor, the controller shows it centred on the pointer (blank override cursor pushed, popped on every exit path as in T11), and on every pointer move outside a stroke re-centres it on the pointer, clamped to the screen.
    - `beginStroke()` freezes the window at its current position instead of re-centring on the press (the reveal path); `endStroke()`/`endStrokeWithoutRelease()` end the stroke and resume following, without hiding the window. The Puck setting applies as in T14.
    - Mode changes swap the widget in place; the following HUD (T12) is shown as in Crosshair. Today, Crosshair, Reveal and CrosshairReveal behave exactly as before.
  - verify: `GuiTests --gtest_filter=ColorTrackballReveal*:ColorTrackballGlobalMode*`: with WidgetCursor, engage shows the window centred on the pointer and the override cursor is blank; a synthetic move re-centres it; a press freezes it and moves to the puck; release resumes following; setMode swaps the widget; exit hides it and leaves no override cursor.
  - size: M

- [x] M119.P1.T9 — End build: run every suite, write the GUI evidence script, and capture each combination at two bin widths
  - files: `Tests/gui/m119_trackball_layout.py` (new)
  - approach:
    - One `fast` build and full ctest.
    - The script opens Grade and ColorCorrect with `NATRON_COLOR_PANEL_DEV=1`, and for each meaningful layout combination (both bound toggles × three styles × two placements) at the default bin width and at 2× captures a screenshot and records the wheel's diameter and the Hue track's length to `results.txt`.
    - It also checks no paint at a stale geometry on an axis switch (the M44.P7.T8 paint logger), that a drag on each style is one undo step, and that Ctrl+E on a second node after leaving the mode in a non-C mode shows "C" in the HUD (T10). For the Feedback axis it also engages global mode with each of Today, Crosshair, Reveal, CrosshairReveal and WidgetCursor, takes one screenshot per mode (C, S, T, V) of the reveal graphic with each Puck setting, and records the override cursor shape before, during and after a stroke.
  - verify: ctest all green; `results.txt` has no FAIL and a wheel diameter and track length for every combination; the shots exist and are listed for the test card, including the four reveal graphics under each Puck setting.
  - size: M

## Phase 119.2: UAT and pick

- [x] M119.P2.T1 — Package the spike AppImage and write the test card
  - files: none in the tree (`build/appimages/`, the test card goes in the PM's message to the user)
  - approach: `package.sh` on the `fast` build. The test card says to launch with `NATRON_COLOR_PANEL_DEV=1`, lists the six axes (the four layout axes plus Feedback and Puck) and what each does, the measured wheel size and track length per combination at the default width, and the questions: which combination wins, which Feedback value and Puck behaviour win (and whether the graphic's 240 px and the HUD's offset are right), and whether the 320 px cap and the 160 px floor are right. Share the key screenshots before the AppImage. Record the user's pick and any tuning in `## Decisions`
- 2026-10-24 — **UAT round 3 (user):** Regular is a bit small and Large too large, and in Regular the sliders don't fill the space. Replace Regular/Large with a preference of 1/2, 1/3, 1/4 or 1/5 of the horizontal space; the wheel sits flush left, the sliders take the rest, and the wheel is never smaller than the sliders' height. The open toggle still becomes a colour swatch: it must be the vertical mirror of the dropdown icon. A knob with its panel open needs clearer identification, e.g. a sunken look over the whole knob. PM calls: default 1/3; the sunken look is one frame around the row and its panel. Phase 119.5 added.
, then re-check Phase 119.3's briefs against it (PLAN-FORMAT.md §5a).
  - verify: the AppImage launches with `--version`; the user's pick is recorded in `## Decisions`.
  - size: S

## Phase 119.3: Keep the winner

Re-planned 2026-10-24 from the user's UAT pick (see ## Decisions). T2, T3, T4 and T6 all edit `ColorTrackballGlobalMode.cpp`/`KnobGuiColor.cpp`, so they run in sequence.

- [x] M119.P3.T1 — Add the trackball feedback and track-size preferences
  - files: `Engine/Settings.h`, `Engine/Settings.cpp`
  - approach: two `KnobChoice`s on `_uiPage`, declared like `_useCursorPositionIncrements` (`Settings.cpp:625`): `colorTrackballFeedback` {Crosshair + reveal, Widget cursor}, default Crosshair + reveal; `colorTrackballSize` {Regular, Large}, default Regular; typed getters; changes reach the GUI through `settingChanged` (`Settings.cpp:2234`). No other values, no migration (no backward compat).
  - verify: `EngineTests` or `GuiTests` case: defaults read back as Crosshair+reveal and Regular; setting each emits `settingChanged` once; a settings save and reload round-trips both.
  - size: M

- [x] M119.P3.T2 — Drive global-mode feedback from the preference; delete Today, Crosshair, Reveal and the absolute puck
  - files: `Gui/ColorTrackballGlobalMode.{h,cpp}`, `Gui/ColorTrackballReveal.{h,cpp}`, `Gui/ColorPanelLayoutDev.{h,cpp}`, `Tests/ColorTrackballGlobalMode_Test.cpp`, `Tests/ColorTrackballReveal_Test.cpp`
  - approach: replace `ColorPanelLayout::Feedback`/`Puck` with a two-value enum read from T1's setting; a change mid-stroke is still deferred to `endStroke()`. Delete the Today/Crosshair/Reveal branches, `Puck` and the absolute path (`colorTrackballRevealDiscPuck`/`…WheelState` keep only relative). Both values keep the crosshair override and the following HUD exactly as P1.T11/T12/T14/T15 built them.
  - verify: `GuiTests --gtest_filter=ColorTrackballGlobalMode*:ColorTrackballReveal*`: each preference value shows its graphic; flipping the setting mid-stroke takes effect after release; no override cursor is left after exit; `grep -rn 'ColorPanelLayout::Puck\|Puck::\|Feedback::' Gui Tests` is empty (the wheel's and disc's marker keep the name puck).
  - size: L

- [x] M119.P3.T3 — Fix the corrupted following HUD: make it a window from construction, and mask the reveal to its disc
  - files: `Gui/ColorTrackballHud.{h,cpp}`, `Gui/KnobGuiColor.{h,cpp}`, `Gui/ColorTrackballGlobalMode.cpp`, `Tests/ColorTrackballHud_Test.cpp`
  - approach: likely cause: `setFollowCursor(true)` reparents a child widget into a top-level window at runtime (`Gui/ColorTrackballHud.cpp:177-210`), which is mapped but never painted, so it shows stale panel pixels. Build the HUD as a window with `followCursorWindowFlags()` from birth, with an opaque fill (`autoFillBackground`, plus a frame in the QSS role's colour). Delete `setFollowCursor`'s reparenting, the `_preFollow*` restore state, and `attach/detachGlobalModeHud`. After `adjustSize()`/`move()`, `update()` the HUD. Give `ColorTrackballReveal` a circular `setMask` so it is round without a compositor.
  - verify: `GuiTests --gtest_filter=ColorTrackballHud*` (the HUD is a window, not a child of any panel). Xvfb (in T10's script): after engage and a pointer move, `QScreen::grabWindow(0, hud.frameGeometry())` matches `hud.grab()` (mean channel difference under 4) and the region contains the "Mode C" label's text pixels; the reveal shot shows no black corners.
  - size: M

- [x] M119.P3.T4 — Exit global mode on a right-click, with no context menu
  - files: `Gui/ColorTrackballGlobalMode.{h,cpp}`, `Tests/ColorTrackballGlobalMode_Test.cpp`
  - approach: in `handleMousePress`, a `Qt::RightButton` press with no other button held calls `exit()`, the same path as Esc. Set a `_swallowRightClickTail` flag and keep the event filter installed until the matching release and the `QEvent::ContextMenu` have been eaten (or the next unrelated event clears the flag), so nothing under the pointer opens a menu. A right press during a left stroke is ignored, as today.
  - verify: GuiTests: synthetic right press+release+ContextMenu while engaged → `isEngaged()==false`, `QApplication::overrideCursor()==nullptr`, ContextMenu reported as filtered. T10's script right-clicks over a node's properties panel during global mode, checks the mode ended, the cursor was restored and no `QMenu` is visible.
  - size: M

- [x] M119.P3.T5 — Delete the bin-top placement
  - files: `Gui/ColorTrackballBinPanel.{h,cpp}` and `Tests/ColorTrackballBinPanel_Test.cpp` (delete), `Gui/PropertiesBinWrapper.{h,cpp}`, `Gui/KnobGuiColor.cpp` (`binTopPlacement`, :236, :486, :576, :781, :1010), `Tests/CMakeLists.txt`
  - approach: ▾ goes back to only the in-knob panel; Ctrl+E/Ctrl+Up/Down are untouched. Remove `Placement` from `ColorPanelLayout`.
  - verify: `GuiTests --gtest_filter=KnobGuiColor*:ColorTrackballGlobalMode*` green; `grep -rn BinPanel Gui Tests` is empty.
  - size: M

- [x] M119.P3.T6 — Make Custom sliders with both edges extended the only layout, and delete the dev switch
  - files: `Gui/ColorPanelLayoutDev.{h,cpp}` (delete), `Gui/ColorAdjustPanel.{h,cpp}`, `Gui/KnobGuiColor.cpp`, `Gui/ScaleSliderQWidget.{h,cpp}` (drop the vertical path), `Tests/ColorAdjustPanel_Test.cpp`, `Tests/ScaleSliderQWidget_Test.cpp`
  - approach: hard-code Custom + extendLeft + extendRight (`KnobGuiColor.cpp:983-988`; keep `getFullWidthRowWidget`). Delete the Natron and Vertical styles, the dev bar, the env var, the `layoutChanged` rebuild, `setOrientation` and the vertical tests.
  - verify: `GuiTests --gtest_filter=ColorAdjustPanel*:ScaleSliderQWidget*:KnobGuiColor*` green; `grep -rn 'NATRON_COLOR_PANEL_DEV\|ColorPanelLayout' Gui Tests` is empty.
  - size: L

- [x] M119.P3.T7 — Make the whole custom slider a relative drag
  - files: `Gui/ColorEditSlider.{h,cpp}`, `Gui/ColorAdjustPanel.cpp`, `Tests/ColorEditSlider_Test.cpp`
  - approach: a press on the marker no longer starts an absolute drag. Every drag is relative at the gear (minimum track length per a44785f42). Drop the `absolute` argument from `gestureMoved` and its handling in the panel; the SpinBox stays the only way to type a value.
  - verify: `GuiTests --gtest_filter=ColorEditSlider*:ColorAdjustPanel*`: a press on the marker followed by 40 px is the same delta as a 40 px drag elsewhere; a click without a move leaves the value unchanged; one undo step per drag.
  - size: M

- [x] M119.P3.T8 — Build the Large track size: a bigger wheel with r/g/b/a and Hue…Tint stacked to its right
  - files: `Gui/ColorAdjustPanel.{h,cpp}`, `Gui/KnobGuiColor.cpp`, `Gui/KnobGuiValue.{h,cpp}`, `Tests/ColorAdjustPanel_Test.cpp`
  - approach: reading of the user's sketch (`build/appimages/M119-uat-large-layout.png`): in Large the wheel grows to cover the label column down past Multiply/Offset/Gamma; the knob's r/g/b/a lines leave their place above the panel and stack in one column right of the wheel, with Hue…Tint under them; the knob row keeps the label and button cluster. With `colorTrackballSize == Large` and ▾ open, reparent the knob's channel lines (the row's r line and `dimensionLines` g/b/a, `KnobGuiValue.cpp:689-749`) into the top of the panel's right column. The wheel is square at the right column's height, capped at 400 px. Regular is T6's layout. Folding ▾, or switching the setting (live through `settingChanged`, deferred past a gesture), puts the lines back under the same update guard and layout flush as P1.T3. If the bin is narrower than the Large minimum, use the Regular arrangement.
  - verify: `GuiTests --gtest_filter=ColorAdjustPanel*:KnobGuiColor*` on Grade `gain` at 500 px: in Large the "g" slider's left x is right of the wheel's right edge, and the wheel is larger than Regular's (still ≤400). Folding returns "g" to its Regular position; a drag on "g" in Large is one undo step; a 300 px bin falls back to Regular.
  - size: L

- [x] M119.P3.T9 — Redesign the S, T and V reveal discs to show what a drag does, with S following the value live
  - files: `Gui/ColorModeDiscWidget.{h,cpp}`, `Gui/ColorTrackballReveal.cpp`, `Tests/ColorModeDiscWidget_Test.cpp`
  - approach: drop the hue-wheel ring on S/T/V. Ring and fill show one function: the colour a drag towards that point produces (from `Engine/ColorWheelMath.h`, through the display converter); every direction is derived from the gesture's sign, so the disc shows where a drag that way actually goes. S: current hue −30° on the left and +30° on the right (replacing `kHueSpanDegrees = 300`), full saturation at the top, white at the bottom; the reveal feeds the live value each stroke move, and the cache is keyed on hue to 1° so the S disc re-colours as you drag; with an undefined hue (saturation ≈ 0) centre on the knob's last defined hue, else 0°. T: warm orange on the right, cool blue on the left, green and magenta on the vertical axis. V: white top and right, black bottom and left. T and V are static. The puck stays relative.
  - verify: `GuiTests --gtest_filter=ColorModeDiscWidget*`: S's left and right rim samples are 30°±3° from the set hue and its top is more saturated than its bottom; changing the hue by 60° rotates both rim samples; T's right rim is warmer than its left; V's top-right rim is lighter than its bottom-left; each mode samples no plain hue ring.
  - size: L

- [x] M119.P3.T11 — The panel's toggle button shows a drop-up (▴) while the trackball is open
  - files: `Gui/KnobGuiColor.{h,cpp}` (the ▾ toggle next to the 4/swatch buttons), its icon resources if it uses one, `Tests/KnobGuiColor_Test.cpp` (or the GUI test that covers the toggle)
  - approach: while the in-knob panel is shown, the toggle displays an up-pointing chevron (▴) with tooltip "Hide the colour panel"; folded, it shows ▾ as today. Today the open state shows a colour swatch, which reads as a colour button, so users don't see how to close the panel. Find where the open state swaps the button's look and replace it; keep the button's size and position so nothing reflows. Runs before T10, which screenshots it.
  - verify: GuiTests: the toggle's icon/text differs between folded and open and matches ▾/▴ (compare against the resources used, or the text glyph); its tooltip changes; clicking it while open folds the panel. T10's script captures both states.
  - size: M

- [ ] ~~M119.P3.T10 — End build, refresh the evidence script to the final layout, and package the approval AppImage~~ (superseded 2026-10-24: UAT round 2 feedback; the end build moves to M119.P4.T4)
  - files: `Tests/gui/m119_trackball_layout.py`
  - approach: drop every dev-switch and bin-top combination. Capture Regular and Large at default and 2× width; both feedback preferences; S/T/V reveal shots including S mid-drag; T3's HUD pixel check; T4's right-click check; keep the size, undo and no-flash checks. One `fast` build, full ctest, `package.sh`; share screenshots before the AppImage.
  - verify: ctest all green; `results.txt` has no FAIL; the user approves on the AppImage.
  - size: M

## Phase 119.4: Split HUD, perceptual discs, warp-back (UAT round 2)

Design reference: `build/m119-mockup/` in the lane (mockup.py, NOTES.md, hud-{C,S,T,V}.png, colours-before-after.png), approved by the user with the changes recorded under Decisions.

- [x] M119.P4.T1 — Split the HUD around the disc and retire the square HUD
  - files: the global-mode HUD and reveal classes (`Gui/ColorTrackballGlobalMode.cpp`, `Gui/ColorTrackballReveal.cpp`, the HUD class), `Tests/ColorTrackballReveal_Test.cpp` or the HUD's test
  - approach: direction labels as small dark rounded pills 6 px outside the outer ring, where each direction acts — S: +Hue right, −Hue left, +Sat top, −Sat bottom; T: Warmer right, Cooler left, Less green top, Greener bottom; V: +Val right and top, −Val left and bottom; C: no direction labels. Below the disc one block: knob name only (no node name), the C/S/T/V key caps with the active one lit, the gearing hints with the held modifier lit, and "Esc/Enter: accept   Ctrl+up/down: switch knob". No live readouts. The square following HUD is retired in both feedback modes. The labels are opaque (no compositor), and the reveal window's mask grows to cover them. The C line, white-point crosshair and pucks stay.
  - verify: GuiTests: labels per mode with the right text on the right side; no overlap with the ring; the block shows the knob name and not the node name; the square HUD is gone.
  - size: L

- [x] M119.P4.T2 — Perceptual disc colours
  - files: `Gui/ColorModeDiscWidget.{h,cpp}`, `Gui/ColorWheelWidget.cpp` (ring), `Gui/ColorTrackballGlobalMode.cpp:1316` (display conversion source), `Tests/ColorModeDiscWidget_Test.cpp`
  - approach: per NOTES.md — keep today's maths only to find each direction's hue, rebuild colours in OKLab with set lightness and capped chroma, gamut-map, encode once for display. S reach ±60° (`kHueReachDegrees`), −Sat edge neutral grey. T: four patches with the final constants from NOTES.md (toned down two-thirds back toward the first proposal). V: perceptual grey ramp centred on mid grey reaching its ends exactly at the rim (fixes the clamp at |dx−dy| ≥ 1 and the 0.5 centre, `ColorModeDiscWidget.cpp:198`). The ring change also applies to the properties-panel wheel. When the panel wheel is absent, the disc still gets a display conversion (today it shows raw linear values).
  - verify: GuiTests: S rim ±60°±3°; T warm right, cool left, magenta top, green bottom with a bounded chroma; V centre is mid-grey's display value and the rim reaches black and white; disc renders through a display conversion with no panel wheel.
  - size: L

- [x] M119.P4.T3 — Cursor warps back to the stroke's start on release
  - files: `Gui/ColorTrackballGlobalMode.cpp` (`handleMousePress` ~:1390, release/`endStroke`)
  - approach: record the press position before any edge warp; after `endStroke()` on a real release, `QCursor::setPos` back there in both feedback modes and swallow the synthetic move; the reveal stays put. Keep today's behaviour where the platform refuses the move, and don't warp when the stroke ends by losing the grab.
  - verify: GuiTests or the evidence script: after a drag and release the cursor is at the press position and the reveal did not move.
  - size: M

- [ ] M119.P4.T4 — Build, evidence script and approval AppImage
  - files: `Tests/gui/m119_trackball_layout.py`
  - approach: update the script to the split HUD (per-mode label checks and shots, warp-back check, perceptual colour probes); one fast build with tests; Xvfb run; share screenshots, then the AppImage.
  - verify: ctest green; results.txt no FAIL; the user approves on the AppImage.
  - size: M

## Phase 119.5: Fractional wheel width, filling sliders, toggle icon, sunken open knob (UAT round 3)

- [ ] M119.P5.T1 — Wheel width as a fraction of the row; sliders fill the rest
  - files: the track-size preference in `Engine/Settings.{h,cpp}` (today Regular/Large), `Gui/ColorAdjustPanel*.cpp` (`ColorAdjustPanelPrivate::updateWheelSize`), `Tests/` tests of the panel layout and setting
  - approach: replace Regular/Large with a choice of 1/2, 1/3, 1/4, 1/5 of the panel's horizontal space (default 1/3). The wheel sits flush left; its diameter is that fraction of the available width, but never less than the total height of the slider column beside it. The sliders take all remaining width and stretch with the panel. The setting applies live. No migration of the old value (clean break).
  - verify: GuiTests: at 1/2, 1/3, 1/4 and 1/5 the wheel's width is the fraction of the row within 2 px, flush left; sliders end at the row's right edge; at a narrow width the wheel equals the slider column's height.
  - size: L

- [ ] M119.P5.T2 — The open panel's toggle is the vertical mirror of the dropdown icon, never a colour swatch
  - files: `Gui/KnobGuiColor.cpp` (`refreshAdjustPanelToolTip(bool)`, the swatch/button refresh path), `Gui/Resources/Images/chevrons-up.svg`, `NATRON_PIXMAP_COLOR_ADJUST_DROPUP`
  - approach: find why the button still shows a colour swatch while the panel is open (a swatch refresh on value change likely repaints the button after P3.T11 set the icon) and make the open state always show the up icon. The up icon is the down icon mirrored vertically, generated from the same source so the two match exactly.
  - verify: GuiTests: with the panel open, after a value change and after a slider drag, the button's icon equals the down icon flipped vertically (pixel compare); closed, it is the down icon.
  - size: M

- [ ] M119.P5.T3 — A knob with its panel open reads as one sunken block
  - files: `Gui/KnobGuiColor.cpp`, the panel container, the knob row's widget/styles
  - approach: while the panel is open, the knob's row and its panel are drawn together as one sunken area: a single frame around both with a slightly darker background and an inset edge, so it stands apart from neighbouring knobs. Closed, the row looks as today.
  - verify: GuiTests: open, the frame encloses the knob row and the panel; closed, no frame. Screenshots to the user.
  - size: M

- [ ] M119.P5.T4 — Build, evidence script and approval AppImage
  - files: `Tests/gui/m119_trackball_layout.py`
  - approach: update the size section to the four fractions, add a toggle-icon check and a sunken-block shot; one release build with tests; Xvfb run; share screenshots, then the AppImage.
  - verify: ctest green; results.txt no FAIL; the user approves.
  - size: M

**Verification gate:** full ctest green; `Tests/gui/m119_trackball_layout.py` reports no FAIL, and in it the split HUD labels sit outside the ring per mode, the cursor returns to the stroke's start on release, and a right-click ends global mode with the cursor restored and no context menu; both feedback preferences and all four wheel fractions work live on the AppImage; the wheel takes its fraction of the row (never less than the sliders' height), flush left, and the sliders fill the rest; the open panel's toggle is the mirrored dropdown icon and the open knob reads as one sunken block; the S/T/V discs show the drag result and S follows the value live; no `NATRON_COLOR_PANEL_DEV`, `ColorPanelLayout`, bin-top, vertical-slider or absolute-puck code remains; the user approved on an AppImage.

## Decisions

- 2026-10-24 — **Phase 119.4 built (PM):** release 957ebe3a6 passed 343/343 on both ctest legs; the evidence script passes with no failures (`build/m119-gui-p4c/`) after it was taught to measure the disc rather than the taller reveal window (c2eddca6e). PM calls on the implementer's open points: the block's mode letter follows the selected mode while the pills follow the drag in progress, which differ only during a mid-drag switch, so it stays. Screenshots sent to the user; P4.T4 waits on their approval and the AppImage `build/appimages/M119-957ebe3a6.AppImage`. Rebased onto main (M80) as 9031c3d8f; release gate running.

- 2026-10-24 — **Mockup review (user), build/m119-mockup/:** approved the split HUD (direction labels outside the ring; knob name, C/S/T/V keys and gearing hints underneath; no node name) and the perceptual OKLCh colours with a mid-grey-centred V ramp. Changes: T labels read Warmer/Cooler and Greener/Less green; T's orange/blue/green/magenta stronger, with shorter fades so the four regions read as patches rather than a smooth hue wipe; bring back the C line and pucks if they were lost. S stays centred on the live hue. Live readouts (Hue/Sat values, gear, knob count) are dropped. On release the cursor warps back to the stroke's start in both feedback modes. Revised T mockup shown before the build.
- 2026-10-24 — **UAT round 2 feedback (user), on M119-a0483fb47:** (1) the HUD overlaps the trackball and a square box suits a round wheel badly — split it: direction labels around the disc where they act (S: right "+Hue", left "−Hue", top "+Sat", bottom "−Sat"; likewise per mode), then underneath the knob name (no node name), then the C/S/V/T and gearing shortcut hints; (2) S's left/right colours move to ±60° hue (±30° look too similar); (3) the trackball colours look garish — use a perceptual/display-referred rendering, and V's ramp needs at least gamma correction; (4) on release the cursor warps back to where the stroke started instead of the display snapping to the release position; (5) show a rendering of the HUD and colour changes before the next build.
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
- 2026-10-24 — **User's pick at P2.T1 UAT** (on `M119-a44785f42.AppImage`): Left and Right bounds both on (the layout stretches into the label column and out to the row's last button). Style: Custom sliders, with one change — drop click-on-indicator; the whole slider is a relative drag. Placement: In the knob (the user's annotated screenshot shows it; bin-top was not chosen). Puck: relative only. Feedback: Crosshair + reveal and Widget cursor both kept, as a user preference (the other Feedback values go). New: a Regular/Large track-size preference for the stretched layout — in Large the trackball is bigger, with the RGBA channel sliders and the custom H/S/V/Temp/Tint sliders stacked beside it, right of the wheel (sketch: `build/appimages/M119-uat-large-layout.png`); Regular is today's pick. Bugs: the HUD shows up corrupted (visible in the evidence shots). Redesign of the S/T/V global-mode reveal rings: no plain coloured rings; each ring shows what the drag does — S: left ≈30° hue to one side, right ≈30° to the other, full saturation at top, white at bottom, and in S mode the ring's colours shift live as you drag; T: warm orange right, cool blue left, green and magenta top and bottom; V: white top and right, black bottom and left. Wheel size bounds (160/320 px) and the reveal's 240 px were not objected to; they stand.
- 2026-10-24 — **User addition:** a right-click exits global mode.
- 2026-10-24 — **Phase 119.3 re-planned** (consultant, from the UAT pick): ten tasks. Defaults taken where the pick was silent: Large wheel cap 400 px DPI-scaled (Regular stays 160/320); the T disc's vertical axis and every disc direction follow the gesture's sign; S with an undefined hue centres on the knob's last defined hue, else 0°. HUD corruption diagnosis is likely, not proven, so T3's verify is a pixel check.
- 2026-10-24 — **User addition:** while the trackball panel is shown, its toggle is a drop-up (▴), not a colour swatch; closing the panel was unclear. Added as P3.T11, run before T10.
- 2026-10-24 — **P3.T4 (code in the right-click commit):** engaging global mode needs a real `Gui` and a built `KnobGuiColor` panel, which no GuiTest fixture provides, so the engaged right-click cases (mode ends, cursor restored, release and menu eaten; ignored during a left stroke) are covered end to end by T10's Xvfb script; GuiTests cover the tail-swallowing class in isolation.
- 2026-10-24 — **Large layout (user):** the wheel sits on the left beside all nine slider lines (r/g/b/a, then Hue…Tint) on the right; the following knobs stay below. The wheel's diameter is bounded by the width left after the slider column, so in Large the beside-the-wheel slider tracks shrink to a ~70 px minimum and the wheel takes the width (~330 px at a 500 px bin against Regular's ~265), capped at 400 px DPI-scaled. The 400 px cap needs a per-instance cap on `ColorWheelWidget` (today `kWheelMaxPx` = 320).
