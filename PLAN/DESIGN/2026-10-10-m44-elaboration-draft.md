# M44 elaboration draft: trackball colour editing

2026-10-10. This is a draft task breakdown for `MILESTONES/M44-trackball-colour-editing.md`. It applies every `## Decisions` entry, and the "Hybrid round 2" entry wins wherever entries conflict. It replaces the draft Phase 44.1–44.5 in `DESIGN/2026-10-10-trackball-colour-editing.md`. That doc is still the reference for the luma–chroma split (§3.3), Kelvin/tint (§3.4), roles (§4.1), undo/render (§4.5) and the scouting table (§1). The hybrid mockup (`build/m44-mockups/mockup.py`, tab H) is the reference for rates and behaviour. It is not to be ported line by line. Code citations are from `264ebf9b4`.

## Design notes

### Chroma plane: CIE xy around the white point, through a projective map

**Choice.** The wheel draws and edits chroma in **CIE 1931 xy coordinates relative to the working white**. The working → XYZ matrix comes from OCIO (P2.T1), uniformly scaled so that its Y row equals the luma weights **w** (`M' = M / (M_Y·1)`). Scaling every row by the same factor leaves xy unchanged.

- **Ratio roles (Gain, Colour, Power's effect vector).** The plane point of a knob **c** is `q = xy(M'c) − xy_w`. Chromaticity does not depend on scale, so q is independent of Y, which makes it the right "relative chroma" for a multiplicative knob.
- **Offset role.** The plane point is the chromaticity of what the offset does to 18 % grey: `q = xy(M'(0.18·1 + o)) − xy_w`. On mid-grey an offset acts like the gain `1 + o/0.18`, so the gain and offset wheels share one geometry. When `0.18 + w·o ≤ 0`, or X+Y+Z ≤ 0, the plane falls back to the linear Jacobian of the same map at white. That plane is continuous with the projective one at small chroma.
- **Inverse.** For a target plane point q and the luminance Y held from gesture start (Y_p = 0.18 + w·o for offsets), the knob is rebuilt as `xyY → XYZ → c = M'⁻¹ XYZ` (offsets subtract 0.18·1 afterwards).

**Why it gives the xy-shaped triangle.** XYZ is linear in c, and xy = (X, Y)/(X+Y+Z) is a projective map. Projective maps send straight lines to straight lines. The gamut edges `c_i = 0` are therefore straight segments, and they meet exactly at the primaries' xy coordinates. The triangle *is* the chromaticity-diagram triangle, translated so that white sits at the origin. Checked numerically for ACEScg in the dev container: the sides are 0.767 / 0.787 / 0.636 xy units, against 4.6 / 19.4 / 20.7 for the mockup's hexcone triangle. The primaries sit at hue angles 353.5° (R), 107.7° (G) and 236.6° (B), close to the familiar 0/120/240°, so the hue ring still reads naturally.

**Why hue and sat edits stay Y-exact.** Every edit (trackball translation, hue rotation about white, saturation along the radius, temp/tint) changes only q, and the knob is always rebuilt at the Y held from gesture start. M' has **w** as its Y row, so `w·c = Y` holds by construction, to one rounding. The numeric check rebuilt from (x, y, Y) = (0.30, 0.36, 1.3) with ΔY = 0.0. The module keeps the old (a, b) split (`F`, `E`) for value edits (§3.3), for the Y ≤ 0 guard, and for the Jacobian fallback.

**Rejected.** A linear opponent plane, or a linear 2×2 warp of (a, b), cannot reproduce the triangle. A linear map that fixes white has 4 degrees of freedom and cannot send three arbitrary vertices to their xy positions. It would also be badly wrong far from white, where the blue vertex lies. CIE 1976 u′v′ is also projective, so it would keep straight edges and (for ACEScg) give sides of 0.51 / 0.48 / 0.59. It is a one-constant swap in the module if UAT prefers it. The user asked for xy, so xy is the default.

**Consequences, stated rather than hidden.**
- The Hue field reads the xy dominant-direction angle, not HSV hue. Red is about 353° in ACEScg, not 0°.
- Saturation is the distance from white in xy units, so the Sat slider's unit is xy distance (shown ×1000).
- Zoom ("rim") is in xy units.
- The mockup's gain rim of 0.25 relative chroma is about 0.031–0.040 xy around the wheel.

**Re-centring.** The wheel is a relative device (round 2):
- **At press.** The plane is anchored at the knob's current q (the "anchor"). The disc preview, the gamut triangle, the locus and the white marker are all drawn relative to that anchor, and the puck starts at the centre.
- **During a drag.** The frame stays fixed, and the puck shows anchor + Δ. Past the rim, the puck parks on the edge with a hollow, outward-pointing look.
- **On release.** The frame re-anchors on the new value and the puck returns to the centre.
- **White off the wheel.** The white marker (neutral) is drawn as a crosshair. When it falls outside the wheel, it is clamped to the rim with a caret pointing toward it.
- **Hue ring.** The ring stays an absolute hue legend about white, whatever the anchor. A press on the ring sets the hue to that angle (rotation about white at constant saturation and Y), and dragging along the ring keeps setting it, as one undo step.

### Knob roles

- **Roles.** `ColorKnobRoleEnum { Auto, Gain, Offset, Colour, Power }` lives in the new pure module `Engine/ColorWheelMath.h`, not in `Global/Enums.h` (an M24 file).
- **Storage.** `KnobColor` carries it as GUI-only metadata: not serialised, not hashed.
- **Native declarations.**
  - Grade: `blackPoint`, `black` and `offset` are Offset. `whitePoint`, `white` and `multiply` are Gain. `gamma` is Power.
  - ColorCorrect: `<Group>Saturation`, `<Group>Contrast` and `<Group>Gain` are Gain, `<Group>Offset` is Offset, and `<Group>Gamma` is Power, for each of the Master, Shadows, Midtones and Highlights groups.
  - Both are declared through the existing helpers `addGradeColorKnob` (`Engine/Nodes/Color/Grade.cpp:167`) and `addScaleKnob` (`Engine/Nodes/Color/ColorCorrect.cpp:295`).
- **Inference.** Everything else (OFX, Python, Constant and the keyers) stays Auto and goes through `inferColorRole(defaults, displayMin, scriptName)`:
  - all defaults 1 → Gain
  - all defaults 0 with displayMin < 0 → Offset
  - script name containing "gamma" with default 1 → Power
  - anything else → Colour
- **Alpha tracks luma on value edits only.** Gain and Colour multiply alpha by Y₁/Y₀. Offset adds Y₁ − Y₀. Power applies the factor in the effect domain (e_a = 0.18^(1/γ_a)), then maps back. Hue, sat, temp and tint never touch alpha, and 3-dim knobs have no alpha.

### Last-touched colour knob and the target

- **Nothing tracks this today.** I found no focus or "last edited" tracking in `KnobGui`, `KnobGuiContainerHelper` or `Gui`.
- **New tracker.** `ColorKnobTouchTracker` (Gui-owned) installs one app-wide event filter for `MouseButtonPress`, `Wheel` and `FocusIn`, and walks the target's parent chain to a widget tagged with a dynamic property.
  - `KnobGuiColor` tags its own per-knob container (`containerLayout->widget()` in `addExtraWidgets`, which is KnobGuiValue's `_imp->container`) and its below-row panel. No `KnobGuiValue` internals need exposing.
  - Shared field containers on multi-knob lines are not tagged, so the tag is never ambiguous.
  - Undoable edits pushed from `KnobGuiColor` (panel, swatch, global mode) also call `touch()`.
  - The tracker maps a `KnobGuiContainerI*` (the panel) to a `KnobGuiWPtr`, and a lookup that fails validation drops the entry.
- **Topmost node.** This is the first entry of `Gui::getVisiblePanels()` (`Gui/Gui15.cpp:401`, front = top of the properties bin, as `putSettingsPanelFirst` maintains it) that is a docked `NodeSettingsPanel`. Floating and non-node panels are skipped.
- **Target resolution**, a pure function tested in GuiTests:
  1. The last-touched knob, if it still belongs to that panel and is visible and enabled.
  2. Otherwise, plugin `net.sf.openfx.GradePlugin` → `white` (label "Gain").
  3. Otherwise, plugin `net.sf.openfx.ColorCorrectPlugin` → `MasterGain`.
  4. Otherwise, the first non-simplified, non-secret `KnobColor` in `getKnobsMapping()` order.
  5. None → nothing happens, silently.
- **Knob order.** Ctrl+Up/Down and Ctrl+scroll walk the same ordered list with no wrap. At either end they stop, and the HUD shows "first/last colour knob".

### Global mode, HUD and viewer border

- **Controller.** `ColorTrackballGlobalMode` is created once per `Gui`. While engaged it installs an app-level event filter.
  - **Strokes.** It adjusts only while a button is held. A press anywhere (any widget, any window) starts a stroke and calls `grabMouse()` on the HUD. Moves send `(mode, dx, dy, gear)` to the edit layer. Release ends the stroke.
  - **Undo.** Each stroke is one undo step (Q4).
  - **Edge warp.** Near a screen edge the cursor is warped back with `QCursor::setPos`, and the synthetic move is ignored. Where warping is impossible (Wayland) it is a no-op.
  - **Keys.** C, T and V pick the mode, Ctrl+Up/Down switches knob, and Esc or Enter accepts and exits. Ctrl+Z and Ctrl+Shift+Z pass through between strokes. Everything else is swallowed.
  - **Exit on teardown.** The controller exits silently when the target's `KnobGui` or panel is destroyed.
- **HUD.** It lives **in the properties bin**: a widget inserted into `PropertiesBinWrapper`'s top-level `QVBoxLayout` (built in `GuiPrivate::createPropertiesBinGui`, `Gui/GuiPrivate.cpp:307-381`), between the buttons row and the scroll area. It therefore stays visible however the bin is scrolled, and it is hidden when the mode is off. It shows:
  - the node and knob name, with n/N
  - the mode, and what x and y do
  - the live values, Y, A and the gear
  - the key legend
  The target row is highlighted, and `ensureWidgetVisible` scrolls it into view.
- **Viewer border.** A transparent, mouse-transparent overlay `QWidget` is parented to each `ViewerTab` and kept on the `ViewerGL` geometry (`ViewerTab::getViewer()`) through an event filter on resize and move. It draws a 3 px rectangle in the user's selection colour (`Settings::getSelectionColor`, read only). The overlay is owned by the global-mode code, so `ViewerTab` and `ViewerGL` need no changes.
- **Shortcut.** Ctrl+G is a new `kShortcutGroupGlobal` action with `Qt::ApplicationShortcut` context, so it also works in floating windows. **Conflict:** Ctrl+G is already NodeGraph "Make group" (`Gui/GuiApplicationManager10.cpp:956`). Two live bindings on one key are ambiguous in Qt, and neither fires (Q1).
- **Row button.** A target button sits at the end of every non-simplified colour row and engages global mode on that knob (earlier hybrid decision; round 2 did not remove it).

### Rates (k = 1; Ctrl ×0.1, Shift ×4, Ctrl+Shift ×0.01, read on every move)

| Gesture | Rate |
|---|---|
| Wheel trackball | The puck tracks the cursor: Δq = Δpx · rim/R_px. The default rim is 10 % of the mockup-equivalent: about 0.0035 xy for every role (Q2). Scrolling zooms ×2^(−¼) per notch, between 0.0002 and 1.0 xy. |
| Saturation (Sat slider relative drag, global C y-axis) | 3.5e-5 xy/px: 10 % of the mockup's global-C rate, converted through the ≈0.14 xy-per-unit Jacobian. |
| Hue (global C x-axis) | 0.5°/px |
| Value (V lock, global V = dx − dy, Value slider relative drag) | Gain, Colour and Power: 1/100 stop/px. Offset: 7e-4 Y/px (the mockup's rim/72). |
| Temp/tint (T lock, global T) | Gain, Colour and Power: 0.5 mired/px (x, right = warm) and 0.25 tint/px (y, up = magenta). Offset: 7e-4/px along the linear axes. |
| Slider marker drag | Absolute and not geared. Pressing anywhere else on the slider, or on an end chevron, drags relatively and is geared. |

All rates live in one `TrackballRates` table in the maths module, so UAT tuning is a one-line change.

**Not carried over from the mockup:** the gear readout, rim readout and rim ± buttons (removed by the user); the H and S keys; the S-edge readout, the Y line and the grey preview (mockup extras: the Value field shows Y). The old `ColorSelectorWidget` popup stays only for simplified/viewer colour knobs (`_useSimplifiedUI`, `Gui/KnobGuiColor.cpp:217`). Everywhere else, the panel's Values tab replaces it.

## Phase 44.1: Colour maths (pure, no Qt, no OCIO)

All of this phase lives in `Engine/ColorWheelMath.{h,cpp}`, which includes nothing from Qt or OCIO and takes the luma weights and the XYZ matrix as plain doubles. Tests go in the **`Tests`** executable: add `Tests/ColorWheelMath_Test.cpp` to `Tests_SOURCES` in `Tests/CMakeLists.txt`, then run `ctest --test-dir build/debug -R ColorWheelMath` inside `tools/ci/local/devshell.sh`, after `tools/ci/local/build.sh debug`. Engine sources are globbed (`Engine/CMakeLists.txt:37`), so new files need a re-configure, not a list edit.

- [ ] M44.P1.T1 — Add the luma basis, roles and value edits with alpha tracking
  - files: `Engine/ColorWheelMath.h` (new), `Engine/ColorWheelMath.cpp` (new), `Tests/ColorWheelMath_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - `ColorKnobRoleEnum { eColorKnobRoleAuto, Gain, Offset, Colour, Power }`.
    - `LumaBasis` is built from `w[3]` (renormalised so Σ = 1) and holds F, E (design doc §3.3) and `M'` (the XYZ matrix scaled so its Y row is w). A matrix-less constructor is used for fallback configs.
    - `KnobColorValue { double rgb[3]; double a; bool hasAlpha; }`.
    - Functions: `split` / `join` (Y, a, b) and `luma`.
    - `scaleValue(role, basis, v, factor)`, `addValue`, and `setValue(role, basis, v, Y)`, each applying the round-2 alpha rule (Design notes, Knob roles).
    - `resetValue(role)` to 1 or 0; for Colour it is a no-op.
    - Y ≤ 1e-6 on a ratio role is reported as `frozen`: value edits switch to additive and chroma edits return `valid = false`.
    - Doubles throughout, never clamped.
  - verify: `ctest -R ColorWheelMath`.
    - split∘join round-trips to 1e-12 under the AP1 and Rec.709 weights.
    - A one-stop value edit on Gain (1,1,1,1) gives (2,2,2,2).
    - Offset +0.1 on (0,0,0,0) gives (0.1,0.1,0.1,0.1).
    - Value edits keep (a, b)/Y (ratio roles) or (a, b) (offset).
    - A 3-dim knob has no alpha.
    - The Y ≤ 0 ratio case is frozen, and an additive value edit escapes it.
    - HDR (gain 8) and negative (offset −0.3) inputs survive.
  - size: M

- [ ] M44.P1.T2 — Add the xy chroma plane: projective map, inverse at fixed Y, offset pivot, Jacobian fallback, hue/sat about white, plane overlays
  - files: `Engine/ColorWheelMath.h`, `Engine/ColorWheelMath.cpp`, `Tests/ColorWheelMath_Test.cpp`
  - approach: Implement the Design notes, Chroma plane section.
    - `PlanePoint toPlane(role, basis, v)` returns q relative to xy_w plus `valid`. The offset role pivots at 0.18 and falls back to the Jacobian when 0.18 + w·o ≤ 0 or X+Y+Z ≤ 0.
    - `fromPlane(role, basis, v, q, Yheld)` keeps Y and alpha.
    - `translate(…, dq)` (trackball), `rotateHueAboutWhite(…, dθ)`, `setHue(…, θ)`, `setSaturation(…, s, rememberedHue)` and `moveSaturation(…, ds, rememberedHue)` (growing along the remembered hue from s = 0).
    - Pure overlay helpers for the widget:
      - `gamutTriangle(basis)`: three plane points from the primaries' xy.
      - `whitePoint(anchor)`, which is −anchor.
      - `clampToDisc(p, rim) → {p', offDisc, direction}`, used by both the white-marker caret and puck parking.
    - The xy/u′v′ choice is one `ChromaticityKind` constant, defaulting to xy.
  - verify: `ctest -R ColorWheelMath`.
    - On ACEScg, every translate, rotate and saturation edit keeps w·rgb within 1e-12 relative, across 1000 random gains in [0.2, 4]³ and offsets in [−0.1, 0.1]³.
    - toPlane∘fromPlane round-trips to 1e-10.
    - Neutral maps to q = 0.
    - The triangle's sides for AP1 are 0.767 / 0.787 / 0.636 (±1e-3), and every point on an edge has one channel equal to 0 (±1e-9).
    - A 360° rotation is the identity.
    - The offset fallback engages at 0.18 + w·o = −0.01 and stays Y-exact.
    - `clampToDisc` returns the direction to an off-disc point.
  - size: M

- [ ] M44.P1.T3 — Add Planckian temperature/tint and the offset-role linear axes
  - files: `Engine/ColorWheelMath.h`, `Engine/ColorWheelMath.cpp`, `Tests/ColorWheelMath_Test.cpp`
  - approach: Port the mockup's model (`mockup.py:49-147`).
    - The Kim et al. locus is valid for 1667–25000 K, worked in CIE 1960 uv, with Newton inversion to (mired, Duv).
    - Temp = mired − mired(white), with + = warmer. Tint = −1000·ΔDuv, with + = magenta.
    - To set a value: go back along the locus with the Duv offset, rebuild XYZ at the held Y, then convert through M'⁻¹. This is exact because M′'s Y row is w, so the mockup's "add a neutral" correction becomes an assertion.
    - Out of domain (a channel ≤ 0, |Duv| > 0.05, or a mired value off the locus): `valid = false`. The caller then uses `linearTempTintAxes(basis)`, the locus tangent and normal at white mapped into the plane, which are also the offset-role axes.
    - `drawLocus(basis, anchor)` returns a polyline in plane coordinates for the widget.
  - verify: `ctest -R ColorWheelMath`.
    - Neutral reads (0, 0).
    - Kim xy at 2856 K, 5003 K and 6504 K matches the CIE tables to 1e-4.
    - Every temp/tint edit keeps Y within 1e-12 relative.
    - An edit followed by its inverse returns the input to 1e-9.
    - The offset axes satisfy w·join(0, axis) = 0.
    - Out-of-domain inputs return invalid and never produce NaN.
  - size: M

- [ ] M44.P1.T4 — Add the Power role (gamma through the mid-grey effect vector) and role inference
  - files: `Engine/ColorWheelMath.h`, `Engine/ColorWheelMath.cpp`, `Tests/ColorWheelMath_Test.cpp`
  - approach:
    - `powerToEffect(γ) = 0.18^(1/γ)` and its inverse `γ = ln 0.18 / ln e`. e ≤ 0, e ≥ 1, or γ ≤ 0 is invalid.
    - Every Power-role edit wraps the Gain edit on the effect vector, alpha included.
    - `inferColorRole(defaults, displayMin, scriptName)` applies the four rules in the Design notes (Knob roles).
  - verify: `ctest -R ColorWheelMath`.
    - Hue, sat and temp edits on γ = (1, 1, 1.2) keep Y(0.18^(1/γ)) within 1e-12 relative.
    - γ = 1 maps to 0.18 and back.
    - The inference table is tested row by row: Grade's seven knobs (by default, range and name), Constant's colour, and an OFX-style `gamma` param.
  - size: M

- [ ] M44.P1.T5 — Add the gesture model: gears, the rates table, slider absolute/relative mapping, and global-mode axis mapping
  - files: `Engine/ColorWheelMath.h`, `Engine/ColorWheelMath.cpp`, `Tests/ColorWheelMath_Test.cpp`
  - approach:
    - `double gear(bool ctrl, bool shift)` gives ×0.1, ×4 and ×0.01.
    - `TrackballRates` holds the table in the Design notes (Rates), including the 10 % saturation rate and per-role defaults.
    - `enum ColorAxis { Trackball, Hue, Sat, Value, TempTint, Alpha }`.
    - `ColorEdit { axis, d1, d2, absolute }` is applied by `applyEdit(role, basis, v, edit, rememberedHue)` (the single dispatcher the widgets and global mode call).
    - `globalModeEdit(mode ∈ {C, T, V}, dx, dy, gear, role, rates)`:
      - C: x = hue and y = sat (up = more).
      - T: x = temp and y = tint (up = magenta).
      - V: dx − dy.
    - `sliderMapping { range, unit }` per (axis, role), from the hybrid table: Hue 0–360°; Sat 0→rim; Value ±3 stops (Gain) or ±0.25 (Offset); Temp ±100 mired or ±50 mC; Tint ±50; Alpha 0–4 or ±0.25. It also provides `valueToMarker` / `markerToValue` and the chevron state past either end.
  - verify: `ctest -R ColorWheelMath`.
    - The gear table is checked.
    - A 100 px global-C y-drag at k = 1 changes saturation by 3.5e-3 xy, and Ctrl gives 3.5e-4.
    - Global V on (dx, dy) = (10, −10) gives +20 px of value.
    - Global T keeps Y exact.
    - Slider marker round-trips.
    - Values past either end report a chevron.
  - size: M

## Phase 44.2: Engine plumbing

- [ ] M44.P2.T1 — Expose the working space's luma weights and XYZ matrix from OCIO
  - files: `Engine/ProjectColorManagement.h`, `Engine/ProjectColorManagement.cpp`, `Tests/ProjectColorManagement_Test.cpp`
  - approach:
    - Add `bool getWorkingToXYZ(double m[9]) const` and `void getWorkingLuma(double w[3]) const`.
    - Probe `getConversionProcessor(working, resolveRoleOrName("cie_xyz_d65_interchange"))` with unit vectors.
    - Check linearity: additivity and 2× scaling, to 1e-5.
    - Fall back to the config's `getDefaultLumaCoefs()`, then to Rec.709. A fallback has no matrix, so temp/tint uses the linear axes.
    - Renormalise w to Σ = 1.
    - Cache under `_mutex`, invalidated by `setWorkingSpace` and by the config change, next to `rebuildPickingProcessors`.
    - When no project exists, the Gui side passes the ACEScg constants from `Engine/Nodes/Image/ColorMath.h`.
  - verify: `ctest -R ProjectColorManagement`.
    - On the built-in ACES 2.0 Studio config with ACEScg working, w matches the AP1 weights to 1e-4, and the matrix's xy primaries match AP1 to 1e-4.
    - A lin_rec709 working space gives the Rec.709 weights.
    - A log working space reports a non-linear probe and the fallback weights.
    - A config change refreshes the cache.
  - size: M

- [ ] M44.P2.T2 — Add a GUI-only role to `KnobColor` and declare it on Grade and ColorCorrect
  - files: `Engine/KnobTypes.h`, `Engine/KnobTypes.cpp`, `Engine/Nodes/Color/Grade.cpp`, `Engine/Nodes/Color/ColorCorrect.cpp`, `Tests/Native/NativeGrade_Test.cpp`
  - approach:
    - Add `setColorRole` / `getColorRole` to `KnobColor`. Auto resolves through `inferColorRole` from the dimension-0 default, the display minimum and the script name.
    - It is not serialised, not hashed, and not in Python.
    - Add a role parameter to `addGradeColorKnob` and `addScaleKnob`, and pass the roles listed in the Design notes (Knob roles).
    - The enum comes from `Engine/ColorWheelMath.h`, so `Global/Enums.h` stays untouched.
  - verify: `ctest -R "NativeGrade|NativeColorCorrect|ColorWheelMath"`.
    - A new case creates a Grade and a ColorCorrect and checks all seven Grade roles and the 4×5 ColorCorrect roles.
    - A Constant's colour infers Colour.
    - Existing render and parity tests are unchanged.
  - size: M

## Phase 44.3: Standalone widgets (GuiTests, offscreen)

These widgets know nothing about knobs. Tests go in the **`GuiTests`** executable: add each file to `GuiTests_SOURCES` in `Tests/CMakeLists.txt`, then run `build/debug/Tests/GuiTests --gtest_filter=<Suite>*`, or `ctest -R <Suite>`. They run on the offscreen platform with no GL, so the debug FP-trap limit does not apply.

- [ ] M44.P3.T1 — Build `ColorWheelWidget` painting: the re-centred xy disc, hue ring, gamut triangle, locus or axes, white marker with off-wheel crosshair and caret, and puck parking
  - files: `Gui/ColorWheelWidget.h` (new), `Gui/ColorWheelWidget.cpp` (new), `Tests/ColorWheelWidget_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - Inputs: `setBasis(LumaBasis)`, `setRole`, `setValue(KnobColorValue)` (which sets the anchor), `setRim`, and `setDisplayConverter(std::function<void(float*, int)>)` for the bulk working → color_picking conversion.
    - **Disc.** Iso-luminant: each pixel shows `fromPlane(anchor + p)` applied to 18 % grey. Pixels with a negative channel are hatched. The disc is cached as a QImage per (size, rim, role, anchor, basis).
    - **Ring.** An absolute hue legend about white.
    - **Overlays.** The P1.T2 triangle; the P1.T3 locus (ratio roles) or axes (offset); and the white marker as a crosshair, clamped to the rim with a caret toward it when off the disc.
    - **Puck.** The puck sits at the centre at rest. `setDragOffset(dq)` draws it, parking it hollow with an outward chevron past the rim.
    - No readouts and no rim buttons.
  - verify: `GuiTests --gtest_filter=ColorWheelWidget*`, rendered offscreen at 160 px.
    - The centre pixel at neutral is achromatic (|r−g|, |g−b| < 2/255) under an identity converter.
    - With an anchor of (2, 1, 1) at a small rim, the white marker reports `offDisc` and the caret points to the angle `whitePoint` predicts (±2°).
    - A parked puck reports `offDisc`.
    - The ACEScg triangle's drawn vertices fall within 1 px of `gamutTriangle`.
    - It writes `grab()` PNGs to the test output directory.
  - size: L

- [ ] M44.P3.T2 — Add wheel input: relative trackball, ring hue set/drag, working V/T held locks, gearing, scroll zoom, double-click reset, re-centre on release
  - files: `Gui/ColorWheelWidget.h`, `Gui/ColorWheelWidget.cpp`, `Tests/ColorWheelWidget_Test.cpp`
  - approach:
    - **Hit test.** A press lands in the disc (trackball) or the ring band (12 px wide, 4 px tolerance).
    - **Ring.** A ring press emits an absolute `Hue` edit at the press angle, and dragging along the ring keeps doing so.
    - **Trackball.** Uses cursor deltas, Δq = Δpx · rim/R_px · gear. The gear is read on every move.
    - **Locks.** V (value, y) and T (temp x / tint y) are held keys over the wheel. The mockup's locks failed; these must work:
      - Take focus on `enterEvent` (`Qt::StrongFocus`).
      - Accept `QEvent::ShortcutOverride` for bare V and T, so the Viewer and Global `QAction`s don't steal them.
      - Ignore `isAutoRepeat()`.
      - Track the pressed set in keyPress/keyRelease. The last pressed key wins, and releasing all of them returns to the trackball from the next move.
      - `grabKeyboard()` for the length of a drag.
    - **Signals.** `gestureStarted()`, `gestureMoved(ColorEdit, QString axisLabel)` and `gestureFinished(bool moved)`. There is no cancel signal: round 2 dropped Esc-cancel, and Esc while dragging just ends the gesture.
    - **Re-centre.** On release the widget re-centres: the owner calls `setValue` with the new value.
    - **Zoom.** Scrolling zooms the rim and never changes the value.
    - **Reset.** A double-click emits a reset-chroma edit.
    - Alt and Meta are ignored.
  - verify: `GuiTests --gtest_filter=ColorWheelWidget*` with QTest events.
    - A 50 px interior drag emits Δq = 50·rim/R_px. With Ctrl it is ×0.1, with Shift ×4, and with Ctrl+Shift ×0.01.
    - Pressing Ctrl halfway through causes no jump.
    - A ring click at 90° emits an absolute hue of 90° ±1°.
    - V held (press sent before the mouse press, focus from a synthetic Enter) gives a Value edit from dy only.
    - A V key press during the drag switches the axis from the next move.
    - An auto-repeat V is ignored.
    - A sent `ShortcutOverride` for V is accepted.
    - T gives TempTint.
    - Scrolling changes the rim only.
    - A double-click emits a reset.
  - size: L

- [ ] M44.P3.T3 — Build `ColorEditSlider`: the absolute marker, relative elsewhere, end chevrons and double-click reset
  - files: `Gui/ColorEditSlider.h` (new), `Gui/ColorEditSlider.cpp` (new), `Tests/ColorEditSlider_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - A horizontal track painted with a gradient the caller supplies (hue spectrum, grey → hue, dark → light, blue → amber, green → magenta, black → white).
    - The marker is placed through `sliderMapping` (P1.T5). Past either end it is drawn as a chevron.
    - **Marker drag.** A press on the marker (±4 px) drags absolutely, keeping the grab offset, ungeared, clamped to the range.
    - **Relative drag.** A press elsewhere or on a chevron drags relatively at range/track px × gear, with no jump on press.
    - Hovering the marker highlights it and shows the ↔ cursor.
    - A double-click emits a reset.
    - Same gesture signals as the wheel, plus a typed-field companion: a `QLineEdit` that commits on Enter as Start + End.
  - verify: `GuiTests --gtest_filter=ColorEditSlider*`.
    - A marker drag of 40 px moves the value by exactly 40 px of range, whatever modifiers are held.
    - A relative drag of 80 px with Ctrl moves 8 px of range.
    - A press with no move emits nothing.
    - A value past the end shows a chevron, and pressing it does not snap.
    - A double-click emits a reset.
  - size: M

- [ ] M44.P3.T4 — Build `ColorAdjustPanel`: a Wheel tab (wheel + Hue/Sat/Value/Temp/Tint/Alpha sliders) and a Values tab (unclamped RGB/HSV, hex, palette)
  - files: `Gui/ColorAdjustPanel.h` (new), `Gui/ColorAdjustPanel.cpp` (new), `Tests/ColorAdjustPanel_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - The panel owns the maths calls.
      - In: `setKnobValue(KnobColorValue, nDims, role, LumaBasis, converter)`.
      - Out: `valuesEdited(KnobColorValue, GesturePhase{Start, Move, End}, QString undoLabel)`.
    - Every widget edit goes through `applyEdit` from the gesture-start value plus the accumulated deltas, so rounding never drifts.
    - It re-centres the wheel on End, and keeps the remembered hue for saturation from 0.
    - **Values tab.**
      - Unclamped RGB and HSV fields. A commit that changes only V counts as a value edit, so alpha tracks.
      - A hex field: display-referred, clamped, and labelled so.
      - The 2×12 palette, reusing `ColorSelectorPaletteButton` (`Gui/ColorSelectorWidget.h:50`).
    - **Dimensions.** 1-dim knobs show only Value. 3-dim knobs hide Alpha.
  - verify: `GuiTests --gtest_filter=ColorAdjustPanel*`.
    - On Gain (1.2, 1, 0.8, 1), a +100 px Value relative drag gives RGB×2 and A = 2.
    - A Temp drag keeps w·rgb within 1e-9 relative and leaves A alone.
    - Sat from 0 grows along the remembered hue.
    - Typing Hue on (1.5, 1, 1) keeps Y.
    - Typing 4.0 in the Values tab's R field round-trips as 4.0, not 1.0.
    - One drag emits exactly one Start and one End.
  - size: L

## Phase 44.4: Knob integration

These tasks change the real colour rows. Each task's GUI evidence is a scratch script under `build/m44-gui/` (untracked), run with `Tests/gui/run-gui-test.sh build/m44-gui/<script>.py build/fast/App/Natron` after `tools/ci/local/build.sh fast`. Debug builds cannot run the GUI under Xvfb, because they trap FP exceptions inside llvmpipe.

- [ ] M44.P4.T1 — Add a below-row widget hook to `KnobGuiValue`, and top-align the labels of rows that use it
  - files: `Gui/KnobGuiValue.h`, `Gui/KnobGuiValue.cpp`, `Gui/KnobGui.h`, `Gui/KnobGui.cpp`, `Gui/KnobGuiContainerHelper.cpp`
  - approach:
    - Add a protected virtual `QWidget* createBelowRowWidget(QWidget* parent)`, defaulting to null.
    - When it is non-null, `createWidget` (`Gui/KnobGuiValue.cpp:219`) wraps `_imp->container` and the new widget in a VBox column. The new widget starts hidden.
    - Relax the `QSizePolicy::Fixed` that the row sets on its parent (`:234-236`) for those rows only.
    - Add a virtual `bool wantsTopAlignedLabel() const` (default false). `KnobGuiContainerHelper` ORs `Qt::AlignTop` into `labelAlignment` (`:841-858`) when it is true.
    - Rows with several knobs on the line (`getKnobsCountOnSameLine() > 1`) never get the hook.
    - The defaults change nothing for any existing knob.
  - verify:
    - The full `GuiTests` and `ctest` suites are green.
    - A scratch GUI script makes a Grade, forces a test-only below-row widget visible through a debug-only property, and screenshots it: the row grows, the label stays at the top, and the rows below shift down.
    - With the property unset, the screenshot matches the pre-change layout pixel for pixel.
  - size: M

- [ ] M44.P4.T2 — Wire `ColorAdjustPanel` into `KnobGuiColor` as the dropdown, replacing the popup, with gesture-to-undo plumbing
  - files: `Gui/KnobGuiColor.h`, `Gui/KnobGuiColor.cpp`, `Gui/ColorKnobEditSession.h` (new), `Gui/ColorKnobEditSession.cpp` (new)
  - approach:
    - **Toggle.** `_colorSelectorButton` becomes a checkable ▸/▾ toggle for non-simplified knobs. The `QWidgetAction` popup stays on the simplified path only.
    - **Panel.** Created through `createBelowRowWidget`, with `wantsTopAlignedLabel()` returning true.
    - **Feeds.** Knob values, `getColorRole()`, the P2.T1 basis (ACEScg constants when there is no project) and a converter that uses `workingToColorPicking`. Refreshed on `updateExtraGui` and on config change.
    - **`ColorKnobEditSession`** is reused by the swatch and by global mode:
      - On Start, record the start RGBA and turn draft render on.
      - On Move, push `KnobUndoCommand<double>(start, current, …)` over every dimension, as the slider does (`Gui/KnobGuiValue.cpp:874-945`).
      - On End, turn draft off and render as `onSliderEditingFinished` does. Honour `getRenderOnEditingFinishedOnly()`: no pushes on Move, one on End.
    - **States.** Expressions on an RGB dimension make the panel read-only. A disabled knob disables it. A wheel edit that makes RGB differ unfolds the dimensions.
    - The expanded state is per instance and not persisted.
  - verify: a scratch GUI script on Grade `white`.
    - The toggle shows the panel, Gain role, with the puck at the centre. Toggling again hides it.
    - A synthetic trackball drag changes `white` with w·rgb unchanged (±1e-6), and one `app` undo restores it bit for bit.
    - Typing 4.0 in the Values tab sets 4.0.
    - A plain swatch click still enables picking.
    - Screenshots: collapsed, expanded, mid-drag.
  - size: L

- [ ] M44.P4.T3 — Make each gesture exactly one undo step with a gesture-scoped merge key on `KnobUndoCommand`
  - files: `Gui/KnobUndoCommand.h`, `Gui/ColorKnobEditSession.cpp`
  - approach:
    - Add an optional `quint64 mergeKey` (0 = today's behaviour) to the list constructor.
    - `mergeWith` (`Gui/KnobUndoCommand.h:293-311`) also requires equal keys when either key is non-zero.
    - `ColorKnobEditSession` takes a fresh key from a static counter on Start, and sets the command text to the gesture's label (for example "Gain: trackball", "Gain: value slider (absolute)").
    - Slider paths are untouched.
  - verify: a scratch GUI script.
    - Two consecutive wheel drags on Grade `white` leave two undo steps.
    - Undo restores the pre-drag RGBA exactly.
    - With autokey at frame 5, a drag creates one key and undo removes it.
    - A KnobGuiValue slider drag still merges as before.
    - Existing `GuiTests` and `ctest` stay green.
  - size: M

- [ ] M44.P4.T4 — Make a swatch drag a trackball (with V/T locks) while a click still toggles picking
  - files: `Gui/KnobGuiColor.h`, `Gui/KnobGuiColor.cpp`
  - approach:
    - Move `ColorPickerLabel`'s toggle (`Gui/KnobGuiColor.cpp:98-107`) from press to release when the pointer moved less than `QApplication::startDragDistance()`.
    - Past that distance, run a trackball through `ColorKnobEditSession` at rim/64 per px with the same gears. V and T held keys follow P3.T2's focus and `ShortcutOverride` rules.
    - Update the tooltip to name the drag, the keys and the gears.
  - verify: a scratch GUI script.
    - A 0 px click toggles picking, and a 2 px jitter still counts as a click.
    - A 30 px drag changes chroma, leaves picking alone, keeps Y and records one undo step.
    - V held during the drag changes only Y.
  - size: M

## Phase 44.5: Global trackball mode

- [ ] M44.P5.T1 — Track the last-touched colour knob and resolve the global-mode target
  - files: `Gui/ColorTrackballTarget.h` (new), `Gui/ColorTrackballTarget.cpp` (new), `Gui/KnobGuiColor.cpp`, `Tests/ColorTrackballTarget_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - `ColorKnobTouchTracker`, created lazily per `Gui`, implements the Design notes (Last-touched colour knob and the target): the app-wide filter for press, wheel and focus-in, and the dynamic-property tag that `KnobGuiColor` sets on its container and panel.
    - `KnobGuiColor` calls `touch()` on every session Start.
    - The pure `pickTargetIndex(pluginId, const std::vector<ColorKnobInfo>&, lastTouchedIndex)` implements the fallback chain.
    - `orderedColorKnobs(DockablePanel*)` walks `getKnobsMapping()`.
    - `topmostNodePanel(Gui*)` uses `getVisiblePanels()`, skipping floating and non-node panels.
  - verify:
    - `GuiTests --gtest_filter=ColorTrackballTarget*` tests the pure picker: Grade with nothing touched → `white`; ColorCorrect → `MasterGain`; Constant → its first colour knob; no colour knobs → none; a touched knob wins; a touched knob that is secret or disabled falls through.
    - A scratch GUI script opens Grade and then Blur (Blur on top → none), clicks Grade's `offset` spinbox, puts Grade first, and checks that the resolver returns `offset`.
  - size: M

- [ ] M44.P5.T2 — Build the global-mode controller: engage/exit, click-drag strokes, C/T/V modes, gearing, accept keys, knob switching
  - files: `Gui/ColorTrackballGlobalMode.h` (new), `Gui/ColorTrackballGlobalMode.cpp` (new), `Gui/KnobGuiColor.h`, `Gui/KnobGuiColor.cpp`
  - approach:
    - Implement the Design notes (Global mode, HUD and viewer border), without the HUD and border, which come in P5.T3.
    - `engage(KnobGuiColorPtr)` and `exit()`. The app event filter is installed only while engaged.
    - **Strokes.** A press anywhere starts a `ColorKnobEditSession` stroke, `grabMouse()`s, and turns moves into `globalModeEdit(mode, dx, dy, gear)`. Release ends the stroke: one undo step, labelled "<knob>: global <mode>". No-button moves do nothing.
    - **Modes.** C, T and V pick the mode. The default is C, and the mode is remembered for the session.
    - **Switching.** Ctrl+Up/Down and Ctrl+scroll ends any stroke, then moves to the next or previous knob from `orderedColorKnobs`. It stops at the ends, and expands a collapsed parent group so the row is visible.
    - **Exit.** Esc and Enter accept and exit.
    - **Pass-through.** Ctrl+Z and Ctrl+Shift+Z pass through between strokes, and the next stroke re-reads the value. All other keys and wheel events are swallowed.
    - **Edge warp.** Done through `QCursor::setPos`, with synthetic moves skipped.
    - **Teardown.** Exits on target destruction or panel close.
    - **Row highlight.** The active row's container gets a highlight property, styled through the existing stylesheet, and a `KnobGuiColor` slot toggles it.
    - **API.** `KnobGuiColor` exposes `engageGlobalMode()` for P5.T4.
  - verify: a scratch GUI script engages through the C++ API, reached from Python via a test-only `QObject` method or the P5.T4 button where it already exists.
    - Synthetic press/move/release on the node graph widget changes Grade `white` in C mode with Y kept.
    - A no-button move changes nothing, and the node graph selection is unchanged.
    - T mode keeps Y.
    - V on gain (1,1,1,1) +100 px gives (2,2,2,2).
    - Two strokes leave two undo steps.
    - Ctrl+Down moves to `multiply`.
    - Esc exits, and the knob keeps its value.
  - size: L

- [ ] M44.P5.T3 — Add the properties-bin HUD and the viewer highlight border
  - files: `Gui/ColorTrackballHud.h` (new), `Gui/ColorTrackballHud.cpp` (new), `Gui/ColorTrackballGlobalMode.cpp`, `Tests/ColorTrackballHud_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - **`ColorTrackballHud`** is a plain widget that shows the node and knob (n/N), the mode, the x/y meaning, the live values, Y, A, the gear, the key legend with the active key lit, and an end-of-list message.
      - The controller inserts it at index 1 of `qobject_cast<QVBoxLayout*>(gui->getPropertiesBin()->layout())`, between the buttons row and the scroll area (`Gui/GuiPrivate.cpp:380-381`).
      - It is shown on engage and hidden on exit.
      - `ensureWidgetVisible` keeps the target row in view.
    - **`ViewerHighlightFrame`** is a `WA_TransparentForMouseEvents` overlay on each `ViewerTab` (from `gui->getViewersList()`).
      - An event filter keeps it on `ViewerTab::getViewer()`'s geometry.
      - It paints a 3 px border in `Settings::getSelectionColor` (read only, no edit to `Engine/Settings.*`).
      - Viewers created mid-session get one too.
  - verify:
    - `GuiTests --gtest_filter=ColorTrackballHud*`: the HUD shows the given mode and values, its legend lights the active key, and a frame over a dummy widget tracks a resize.
    - A scratch GUI script engages on Grade `white`: the HUD is visible inside the properties bin above the panels; the border pixel at the viewer's edge equals the selection colour (±2/255); after exit, both are gone.
    - Screenshot of the global mode in T.
  - size: M

- [ ] M44.P5.T4 — Add the Ctrl+G application-wide shortcut and the per-row target button
  - files: `Gui/ActionShortcuts.h`, `Gui/GuiApplicationManager10.cpp`, `Gui/Gui.cpp`, `Gui/KnobGuiColor.cpp`
  - approach:
    - Add `kShortcutIDActionColorTrackball` and its description in `kShortcutGroupGlobal`, registered at Ctrl+G.
    - Per Q1(a), move the default `kShortcutIDActionGraphMakeGroup` to Ctrl+Shift+G at `GuiApplicationManager10.cpp:956`. Both files are outside the M24 list. Q1(b) would need an edit in `Gui/NodeGraph25.cpp` and would be OVERLAP: `Gui/NodeGraph25.cpp`.
    - In `Gui.cpp`, create the `ActionWithShortcut` with `Qt::ApplicationShortcut`, `addAction` it to the main window, and connect it to resolve the target (P5.T1) and engage (P5.T2), or do nothing when there is no target. Pressing it while engaged exits.
    - Add a target button (crosshair icon, checkable while engaged) at the end of every non-simplified colour row.
  - verify: a scratch GUI script.
    - With Grade on top and nothing touched, a synthetic Ctrl+G sent to the node graph engages on `white` and does not create a group.
    - Ctrl+Shift+G with nodes selected makes a group.
    - With Blur on top, Ctrl+G does nothing and logs nothing.
    - Ctrl+G from a floating properties panel works.
    - The row button engages on its own knob.
    - The shortcut editor lists the new action.
  - size: M

## Phase 44.6: GUI evidence and user UAT

- [ ] M44.P6.T1 — Write the M44 GUI evidence script and capture the screenshot set
  - files: `build/m44-gui/m44_trackball.py` (new, untracked scratch, not committed)
  - approach:
    - Use the `Tests/gui/guitest.py` helpers (`run`, `check`, `shot`, `viewer_centre_colour`).
    - Graph: Constant (0.18 grey) → Grade → Viewer.
    - Drive real widgets by object name with synthetic QMouseEvent and QKeyEvent through PySide6.
    - Sections:
      - collapsed rows with target buttons
      - the Gain panel expanded at neutral
      - a trackball mid-drag
      - after release, re-centred, with the white crosshair and its caret off the wheel
      - a ring click setting the hue
      - V and T locks on the wheel
      - absolute and relative Value slider drags, with alpha tracking
      - an Offset (`black`) panel
      - the ACEScg triangle with roughly equal sides, drawn at a zoomed-out rim
      - global mode by Ctrl+G: the HUD in the bin, the viewer border, a C stroke, Ctrl+Down to the next knob, Esc
      - ColorCorrect, where Ctrl+G engages `MasterGain`
    - Checks:
      - the viewer centre's luminance under the working weights stays within 1 % across hue, sat and temp edits
      - each stroke is one undo step
      - undo restores the values
  - verify: `tools/ci/local/build.sh fast`, then `Tests/gui/run-gui-test.sh build/m44-gui/m44_trackball.py build/fast/App/Natron` exits 0. `build/gui-test-out/results.txt` shows no FAIL, and the shots exist for every section. The PR description lists the shots.
  - size: M

- [ ] M44.P6.T2 — Package an AppImage for the user's hands-on test
  - files: none (output: `build/fast/artifacts/*.AppImage`)
  - approach: Run `tools/ci/local/package.sh fast` on the tree that passed P6.T1. Hand the user the AppImage path and a one-screen test card:
    - the wheel gestures and V/T locks
    - slider absolute/relative behaviour
    - Ctrl+G, C/T/V, Ctrl+Up/Down or Ctrl+scroll, Esc/Enter
    - the gearing
    - the Make group shortcut move (per Q1)
    - things to judge: saturation speed, the default zoom, and the triangle shape
  - verify: The AppImage exists and launches (`--version` under `devshell.sh`). The user runs the UAT and records their verdict in the M44 Decisions.
  - size: S

**Verification gate:**
- CI `format`, `lint-ci` and `build-and-test` are green.
- In `Tests`: `ColorWheelMath*`, `ProjectColorManagement*`, and the new NativeGrade/ColorCorrect role cases pass.
- In `GuiTests`: `ColorWheelWidget*`, `ColorEditSlider*`, `ColorAdjustPanel*`, `ColorTrackballTarget*` and `ColorTrackballHud*` pass.
- `Tests/gui/run-gui-test.sh build/m44-gui/m44_trackball.py build/fast/App/Natron` passes, and its screenshots are attached to the PR.
- `tools/ci/local/package.sh fast` produces an AppImage, and **the user's hands-on UAT on it passes**.
- No file on M24's list is touched. If Q1 resolves to (b), the one exception is flagged OVERLAP: `Gui/NodeGraph25.cpp` and sequenced after M24 merges.

## Open questions for the user

1. **Ctrl+G is already NodeGraph "Make group"** (`Gui/GuiApplicationManager10.cpp:956`). An application-wide Ctrl+G cannot coexist with it.
   - (a) The trackball takes Ctrl+G everywhere, and Make group's default moves to Ctrl+Shift+G (free today). Users can rebind either in the shortcut editor.
   - (b) Ctrl+G stays Make group while the node graph has focus, and is the trackball elsewhere. Your usual flow, selecting the Grade in the graph and pressing Ctrl+G, would then make a group. This option is also OVERLAP: `Gui/NodeGraph25.cpp`.
   - (c) Give the trackball a different key.
   - **Recommended: (a).**
2. **What "saturation speed 10 %" covers.**
   - (a) Every saturation change: the Sat slider's relative drag, global C's y-axis, *and* the wheel trackball, through a default zoom 10× tighter. The puck still tracks the cursor, and the gamut triangle starts off the wheel until you scroll out.
   - (b) Only the explicit saturation axes (Sat slider, global C). The wheel keeps the mockup's zoom.
   - **Recommended: (a).** Both are one constant in `TrackballRates`, so UAT can retune.
3. **The viewer border.** "Monitor" is read as the viewer pane.
   - (a) A border on every open viewer pane.
   - (b) A border on the active viewer only.
   - (c) The whole main window, as in the mockup.
   - **Recommended: (a).**
4. **Undo granularity in click-drag global mode.**
   - (a) One undo step per press→release stroke, with Ctrl+Z usable between strokes without leaving the mode.
   - (b) One step per knob per session, as the earlier free-move design had.
   - **Recommended: (a).** It matches "every user action is one undo step", and with Esc no longer cancelling, undo is the way back.
