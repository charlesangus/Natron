# Design: trackball colour editing (M44)

2026-10-10, v1. Pitches for user review before M44 is elaborated. Supersedes the M44 section of `2026-10-10-m44-m55-m56-scoping.md`. That draft proposed modifier-drags on the swatch with plain HSV, which the user has since widened (`M44-trackball-colour-editing.md` Decisions). Citations were checked on this checkout (`2cf878e7f`). Settled elsewhere and not re-argued here: M44 has no dependency on M24, the node-colour button is out of scope, and every user action is one undo step.

## 0. Goals / non-goals

**Goals.**
1. A colour wheel in an inline dropdown panel under every `KnobColor` row, like Nuke's. Angle = hue, radius = saturation, and one x/y drag changes both, like a grading-panel trackball.
2. Separate gestures for hue only, saturation only, value only, and temperature (plus tint, its orthogonal partner).
3. Gearing modifiers for slower and faster drags, with stated multipliers.
4. **Value preservation.** Hue, saturation, temperature and tint edits leave the knob's luminance exactly unchanged in the working space. "Exactly" means to float precision, not perceptually approximately.
5. Correct for what colour knobs actually hold: scene-linear working-space values, which may be negative or above 1. Most of them are gains or offsets, not colours, so the wheel's centre is the knob's neutral point.
6. The plain click on the swatch keeps toggling viewer colour picking (`Gui/KnobGuiColor.cpp:98-107`).

**Non-goals.** The node-colour button (`Gui/DockablePanel.cpp`, M24 territory). Simplified/viewer-toolbar colour knobs (`_useSimplifiedUI`, `Gui/KnobGuiColor.cpp:217`): they keep today's popup. Any change to how nodes *apply* their knobs; the Grade/ColorCorrect kernels are untouched. A vectorscope. Hardware panel (MIDI/HID trackball) input, though §6 keeps the gesture layer input-agnostic so it can come later. Alt and Meta/Super drags: GNOME and KDE use Alt or Super + drag to move windows.

## 1. What exists today (scouted)

| Thing | Where | Relevance |
|---|---|---|
| Swatch | `ColorPickerLabel`, `Gui/KnobGuiColor.h:58-104`, `.cpp:89-201` | `mousePressEvent` toggles picking on *any* press (`:98-107`); no move/release handling. It is painted from `workingToColorPicking` and clamped to [0,1] (`:369-387`). |
| Popup selector | `_colorSelectorButton` + `ColorSelectorWidget` in a `QWidgetAction` (`Gui/KnobGuiColor.cpp:301-334`) | A QtColorTriangle, RGB/HSV/A sliders, hex field, 2×12 palette (`Gui/ColorSelectorWidget.h:100-226`). **It clamps to [0,1] both ways** (`updateColorSelector`, `:531-536`), so opening it on a gain of 2 and touching anything writes 1. It also writes with `eValueChangedReasonNatronInternalEdited` and pushes **no undo command** (`onColorSelectorChanged`, `:340-366`). |
| Row construction | `KnobGuiValue::createWidget` is `FINAL` (`Gui/KnobGuiValue.h:157`). It builds `_imp->container` with an HBox inside the field HBox and calls `addExtraWidgets(containerLayout)` last (`Gui/KnobGuiValue.cpp:219-506`). | No hook exists for a widget *below* the row. The page grid puts the label in column 0, aligned `Qt::AlignRight` with vertical centring (`Gui/KnobGuiContainerHelper.cpp:840-860`), so a tall field would centre the label vertically. Both need small changes (§7, P4.T1). |
| Slider undo pattern | `KnobGuiValue::onSliderValueChanged` → `sliderEditingEnd` (`Gui/KnobGuiValue.cpp:874-945`) | Correction to the brief: the slider pushes a **mergeable `KnobUndoCommand` on every move**. `QUndoStack` compresses consecutive ones by `id()`/`mergeWith` (`Gui/KnobUndoCommand.h:293-311`). One command on release would mishandle autokey, because `redo()` records `_oldKeys` from the curve *before* its own `setValue` (`:236-243`), so keys that live writes created during the drag would survive undo. The catch: two separate drags on the same knob also merge into one step today. `mergeWith` only refuses after a keyframe was added (`:259-262`). |
| Draft render | `ScaleSliderQWidget` turns draft off and emits `editingFinished(hasMoved)` on release (`Gui/ScaleSliderQWidget.cpp:225-232`). `onSliderEditingFinished` honours `getRenderOnEditingFinishedOnly()` and `isAutoProxyEnabled()` (`Gui/KnobGuiValue.cpp:886-899`). | Copy this exactly. |
| Modifier conventions | SpinBox wheel/arrow keys: Shift = ×10 coarser, Ctrl = ÷10 finer (`Gui/SpinBox.cpp:638-645`, `:712-719`). Slider: Ctrl zooms ×10 (finer), Ctrl+Shift ×100 (`Gui/ScaleSliderQWidget.cpp:263-270`). | Gearing should follow suit: **Ctrl = finer** in Natron. |
| HSV | `Engine/Lut.h:44-45` `rgb_to_hsv`/`hsv_to_rgb` | Exists. §3 explains why it is not the editing model. |
| Luminance | `Engine/Nodes/Image/ColorMath.h:55-90`, `LuminanceMathEnum` with Rec709/Rec2020/AP0/AP1/CCIR601/Average/Max | Fallback weights and test oracles. Note that Saturation and ColorCorrect default to Rec709 (`Engine/Nodes/Color/Saturation.cpp:208`) although the default working space is ACEScg. That is outside M44, but it shows that nothing in the Engine knows the working space's luma today. |
| Working space | `Project::getWorkingColorSpace()` (`Engine/Project.cpp:1719`) → `ProjectColorManagement::setWorkingSpace` (`Engine/ProjectColorManagement.cpp:751`). Conversions go through `getConversionProcessor(src, dst)` (`:716-748`) and `workingToColorPicking` (`:790`). Config-change callbacks are `addConfigChangedCallback`. | There is no luma/XYZ accessor. P2.T1 adds one by probing working → `cie_xyz_d65_interchange` (an ACES 2.0 Studio config role) with unit vectors. |
| Colour knobs on native nodes | Grade: `addGradeColorKnob` (`Engine/Nodes/Color/Grade.cpp:166-190`). blackPoint 0, whitePoint 1, black(Lift) 0, white(Gain) 1, multiply 1, offset 0, gamma 1 (`:265-271`). ColorCorrect `addScaleKnob` ×4 groups: saturation 1, contrast 1, gamma 1, gain 1, offset 0 (`Engine/Nodes/Color/ColorCorrect.cpp:400-404`). Also Constant, CheckerBoard, Clamp, ColorLookup, ColorMath, Keyer, ChromaKeyer. | All are 4-dim, range ±DBL_MAX, with display ranges. Their roles (§4.1) can be declared at these call sites. OFX colour params must be inferred. |
| GUI evidence | `Tests/gui/run-gui-test.sh <script.py>` (Xvfb+GLX), `Tests/gui/guitest.py` (`shot()`, `check()`, `run(gen)`). Widget unit tests go in the `GuiTests` binary on the offscreen platform (`Tests/CMakeLists.txt:140-186`). | P5 and the gate. |

## 2. Survey

**Nuke (colour knob wheel button).** The colour-wheel button next to a colour knob expands an *inline* picker under the knob, not a floating dialog. It holds a hue/saturation wheel, a value slider, RGB/HSV/TMI slider sets and the swatch. My recollection of Foundry's docs (not verified on this host) is that a wheel drag sets hue+sat, with Ctrl/Shift variants constraining to hue, saturation or value. *Good:* inline, so the panel stays in context and the viewer stays visible. The wheel is fast for coarse moves. TMI (temperature/magenta/intensity) sliders exist. *Bad:* the maths is HSV, so V = max(r,g,b). Rotating a gain from red to green brightens the image a lot (numbers in §3.1). The wheel's centre is "white at V". That suits a gain of 1, but an offset of 0 sits at V = 0, where S and H are undefined and the wheel does nothing useful. TMI lives in sliders unrelated to the wheel. There is no gearing on the wheel.

**DaVinci Resolve (Primaries — Color Wheels, HDR palette).** Four wheels: Lift, Gamma, Gain, Offset. Each wheel is a trackball: dragging *anywhere* inside moves the puck *relatively* and slowly, without jumping to the click point. A **master ring** around each wheel is jogged horizontally to change that wheel's luminance, so chroma and luminance are physically separate controls. A double-click or the per-wheel reset resets it. Temp/Tint/Saturation/Hue/Lum Mix are separate sliders. The HDR palette (zone wheels) works in a selectable, colour-space-aware working space, with exposure as the ring and per-zone saturation. *Good:* chroma/luminance separation in the gesture itself, relative motion (no jumps), a neutral centre for both multiplicative and additive wheels, and orientation matched to the vectorscope. *Bad:* the default YRGB wheels are only luminance-preserving through "Lum Mix", which defaults to mixed, not exact. Slow sensitivity is a feature on the panel and a frustration with a mouse. Temperature is not on the wheel.

**Baselight (Blackboard trackballs) / Quantel Pablo.** Physical trackballs for chroma with the surrounding ring for luminance, the model the user named. Baselight's Base Grade does balance/temperature in a perceptual-ish space with chromatic adaptation, so temperature behaves like a white-balance change rather than an RGB push. *Good:* one hand gives hue+sat at once, the ring gives value, and temperature is physically motivated. *Bad:* it is hardware-first. The software trackball proxies are tiny and rely on panel-style relative motion.

**Blender (colour picker wheel, Color Balance node).** The picker popup is an HSV (or HSL) wheel plus a value/lightness slider, with HDR values allowed above 1 in the numeric fields. The Color Balance node draws Lift/Gamma/Gain (and ASC-CDL Offset/Power/Slope) as wheels with the value slider beside each, centre = neutral. Recent versions add a white-point temperature/tint pair to Color Balance via chromatic adaptation. *Good:* a centre-is-neutral wheel next to a separate value control is the closest precedent to what we want, and temperature/tint are first-class. *Bad:* HSV hue/sat edits are not luminance-preserving, and the wheel is a popup.

**What we take:** Nuke's *inline* dropdown, Resolve's *relative* puck and *ring/bar for value*, Baselight's *physically-based temperature*, and Blender's *centre = neutral with a separate value control*. What we fix: exact luminance preservation and a correct neutral for additive knobs.

## 3. Colour-space investigation

### 3.1 What "value" must mean here

A colour knob is a *coefficient vector* that a node applies to linear pixels: `out = gain ∘ in`, `out = in + offset`, `out = in^(1/gamma)`. It is rarely a colour you look at. The brightness that matters is the knob's **effect on the image's luminance**, measured in the working space as Y = **w**·rgb, where **w** is the working space's Y row (ACEScg/AP1: 0.2722287, 0.6740818, 0.0536895; Rec.709: 0.2126, 0.7152, 0.0722).

- For a **gain** **g** applied to a neutral pixel k·**1**, the output luminance is k·(**w**·**g**). Keeping **w**·**g** fixed keeps every neutral pixel's luminance exactly. For a non-neutral pixel no per-channel gain change can preserve luminance for *all* pixels. Preserving it for neutrals, and to first order around neutral, is the strongest guarantee a per-channel gain can offer. The doc states this limit plainly rather than over-promising.
- For an **offset** **o**, the luminance shift is **w**·**o** for every pixel, exactly. Keeping **w**·**o** fixed preserves luminance exactly for all pixels.
- For a **gamma** **γ**, the effect is non-linear in the pixel. §4.1's Power role preserves the luminance of mid-grey (0.18), which is exact at the pivot and approximate elsewhere.

So the right "value" is **Y = w·c in the working space**, with *w* taken from the working space rather than assumed. HSV's V = max(r,g,b) is not it. Worked example in ACEScg: a gain of (1.5, 1, 1) has Y = 1.136. Rotating its HSV hue to green gives (1, 1.5, 1), Y = 1.337 (+0.24 stops). Rotating it to blue gives (1, 1, 1.5), Y = 1.027 (−0.15 stops). A half-stop swing from a "hue-only" move is exactly what the user wants removed.

### 3.2 Candidates

| Space | Exact Y kept by hue/sat edits? | HDR > 1 | Negatives | Linear (a gain edit stays a 3×3 matrix) | Hue uniformity | Fit for coefficient knobs |
|---|---|---|---|---|---|---|
| HSV (`Lut.h`) | **No.** V = max, not luminance (§3.1). | V unbounded, OK | S > 1 or NaN-prone; hue jumps | No (piecewise) | Poor (hexcone) | Poor |
| HSL / HSLuv | No / HSLuv yes, but only inside sRGB [0,1] | HSLuv no (gamut-bounded by definition) | No | No | HSLuv good | Reject: bounded to display sRGB |
| CIELab/LCh | **Yes.** L* depends only on Y, so a rotation in a*b* keeps Y. | L* > 100 extrapolates | Cube root needs a signed hack | No (cube roots) | Poor in blue (the well-known purple shift) | OK on maths, but non-linearity buys nothing for coefficients |
| OKLab/OKLCh | **No.** L comes from cube roots of LMS, so a hue turn at fixed L changes Y by a few %. A Y-renormalise step fixes Y but drifts L. | Extrapolates (designed for SDR D65) | Signed cbrt hack | No | Good | Good for *picking display colours*, wrong invariant for us |
| ICtCp / Jzazbz | No (I/Jz ≠ Y) | Yes (PQ, absolute nits) | No | No | Good | Needs a "scene 1.0 = N nits" assumption that a gain has no meaning for. Overkill. |
| **Linear luma–chroma (opponent plane around the achromatic axis)** | **Yes, by construction** | Yes | Yes | **Yes** | Moderate (RGB-hexcone-like angles, smooth) | **Best** |

The deciding argument is linearity. A gain is used linearly, so the *meaningful* geometry on gains is linear too. A perceptual space applied to the coefficient vector (1.2, 1, 0.8) computes something with no physical meaning, a "lightness of a gain". A linear decomposition answers the real question: what this gain does to grey, split into a luminance part and a chroma part. It is closed-form, invertible, cheap, and exact for HDR and negative values. It also matches what Natron's Saturation node already does to pixels: `c' = Y + s·(c − Y)` (`Engine/Nodes/Color/Saturation.cpp:65`).

### 3.3 Recommended model: luma–chroma split with hexcone hue angles

For a knob RGB **c** and working luma weights **w** (Σw = 1):

```
Y      = w · c                                  value (luminance of the knob's effect on grey)
(a, b) = F c,   F = [ 1   -1/2    -1/2  ]        chroma coordinates; F·1 = 0
                    [ 0   √3/2   -√3/2 ]
c      = Y·1 + E (a, b),   E = (2/3)(I − 1 wᵀ) Fᵀ  exact reconstruction (F E = I, wᵀE = 0)
θ      = atan2(b, a)                             hue angle
```

Properties, each a unit test in P1:
- **F does not depend on w.** Pure R, G, B land at exactly 0°, 120°, 240°, and Y/C/M at 60°, 180°, 300°, the same hue numbers HSV shows. The numeric Hue field therefore reads like Nuke's and Natron's existing HSV slider at the cardinal points, and is smooth in between.
- **E depends on w.** That is the only place the working space enters. **wᵀE = 0**, so any edit that only changes (a, b) leaves Y bit-for-bit the same, up to one rounding of the final `Y·1 + E(a,b)` sum.
- **Hue rotation** by Δθ: (a, b) ← R(Δθ)(a, b). **Saturation**: scale (a, b), or move the radius additively from a remembered hue when starting at zero. **Value**: change Y and keep (a, b)/Y (ratio roles) or (a, b) (offset role), §4.1. For a gain, each edit composes to the 3×3 matrix `M = 1wᵀ + E·T·F`, with M·**1** = **1** (neutral stays neutral) and **w**ᵀM = **w**ᵀ (luma preserved).
- **Worked example** (ACEScg). The gain (1.5, 1, 1) has Y = 1.136 and (a, b) = (0.5, 0). Rotated by +240° it becomes (1.109, 1.109, 1.609), still Y = 1.136, against HSV's 1.027.
- **HDR/negatives.** Nothing clamps. When the chroma is large relative to Y, a channel can go negative (a gain with a negative channel). The wheel draws the **gamut-edge contour**, where min(c) = 0, as a thin triangle. The panel also shows the derived *edge saturation* `S_edge = 1 − min(c)/Y`: 1 means a channel touches 0, above 1 means negative. Edits are allowed past it, because a negative gain channel is legitimate in comp.
- **Y ≤ 0 for a ratio role** (a gain pulled to 0 or below). Normalised chroma is undefined there. The puck freezes at its last position with a "Y ≤ 0" badge, hue/sat/temp gestures are disabled, and the value gesture switches to additive until Y > 1e-6.

**Getting w.** P2.T1 adds `ProjectColorManagement::getWorkingLuma(double w[3])` and `getWorkingToXYZ(double m[9])`. They apply `getConversionProcessor(working, "cie_xyz_d65_interchange")` to the three unit vectors and read the matrix's columns. A linearity check (P(e_r) + P(e_g) ≈ P(e_r + e_g), and P(2e) ≈ 2P(e)) catches a non-linear working space (a log space picked by hand), which falls back to the config's default luma coefficients, then Rec.709. The weights are renormalised to Σ = 1, so the D65 interchange's chromatic adaptation does not leave white at Y ≠ 1. The result is cached, refreshed by the config-changed callback, and passed into the maths module as plain numbers, so the module never sees OCIO.

**Rejected alternatives, briefly.** OKLCh with a Y-renormalising step would give better-spaced hues, but it makes a gain edit non-linear and does not make the *knob* more meaningful. It stays possible later as a pure *angle warp* of how hues are laid out on the drawn wheel, with the maths unchanged. "Value-preserving HSV" variants (HSV with V replaced by Y) amount to the hexcone *without* exact linear reconstruction, which is what §3.3 already provides, minus the piecewise seams.

### 3.4 Temperature and tint

Temperature and tint are **a second coordinate system on the same chroma plane**, not a separate state. Polar (θ, ρ) and locus-aligned (temp, tint) both describe (a, b), so they compose with the wheel for free. A temperature drag is a puck move along the drawn locus. Nothing extra is stored on the knob.

- **Gain / Colour roles (exact, Planckian).** Treat the knob as "the colour the working white becomes": XYZ = M_xyz·**c** → chromaticity xy → (CCT, Duv) by Robertson's or Ohno's method. **Temp** = mired(c) − mired(white_working), with positive = warmer/amber. **Tint** = −1000·(Duv(c) − Duv(white_working)), with positive = magenta. Setting either: take the locus xy at the new CCT and offset it by Duv along the locus normal (Kim et al. cubic for xy(T), 1667–25000 K) → XYZ with **Y held at the knob's current w·c** → working RGB. Y is preserved by construction, because the XYZ Y is the same **w**·**c**. ACEScg's white (≈ D60) sits at about 6000 K, so neutral reads Temp 0, Tint 0. Outside the valid domain (|Duv| > 0.05, a CCT out of range, or any channel ≤ 0) the readouts show "—" and the jog switches to the linear axes below.
- **Offset role (linear).** An additive offset has no white point, so temp/tint are the two fixed unit directions in the (a, b) plane given by the locus tangent and normal at the working white, pushed through the gain model's Jacobian at neutral. They are reported in milli-chroma units. Y is exact, since both directions lie in the chroma plane.
- **Power role** uses the gain model on the mid-grey effect vector (§4.1).

Planckian temperature is the clear "improve on Nuke" item. Nuke's TMI temperature is a fixed RGB axis, which drifts green as it goes warm. If the user prefers simplicity, the linear-axes model can be used for every role (open question Q3); the module design does not change.

## 4. Shared semantics (all three pitches)

### 4.1 Knob roles: where the centre is and what the radius means

| Role | Typical knobs | Neutral (wheel centre + value reset) | Puck = | Value V and its gesture | Rim (auto-stepped, §4.2) |
|---|---|---|---|---|---|
| **Gain** (multiplicative) | Grade gain/multiply/whitePoint, ColorCorrect gain/saturation/contrast | chroma 0, **Y = 1** | (a, b)/Y | Y; drag is exposure-like, Y ← Y·2^(k·Δ). Shown in stops too. (a, b)/Y kept, so the puck does not move. | start 0.25 |
| **Offset** (additive) | Grade lift/offset/blackPoint, ColorCorrect offset | chroma 0, **Y = 0** | (a, b) absolute | Y; drag is additive, Y ← Y + k·Δ (adds a neutral). (a, b) kept. | start 0.05 |
| **Colour** (absolute) | Constant/CheckerBoard colour, keyer colours | chroma 0 at the knob's *current* Y (no value reset target) | (a, b)/Y | like Gain (multiplicative). Additive below Y = 1e-6, so black is reachable and escapable. | start 1.0 |
| **Power** (gamma) | Grade gamma, ColorCorrect gamma | chroma 0, **γ = 1** | gain model on **e** = 0.18^(1/γ) (each channel's effect on mid-grey), mapped back γᵢ = ln 0.18 / ln eᵢ | Y(**e**) multiplicative. Mid-grey luminance is preserved exactly by hue/sat/temp. | start 0.25 |

So **the wheel's centre is always the achromatic axis** (a, b) = 0. Multiplicative and additive knobs differ in only two ways: whether the radius is relative to Y, and whether "value" is multiplied or added and resets to 1 or 0. That is why one widget serves both. It also fixes Nuke's dead-at-zero offset wheel: an offset's puck at the centre is (0,0,0), and moving it adds a luminance-neutral chroma offset.

**Role source.** A new `KnobColor::setColorRole(ColorKnobRoleEnum)` is declared explicitly at the native call sites listed in §1. For everything else (OFX params, Python-created knobs) a pure `inferColorRole(defaults, displayMin, scriptName)` decides:
- all defaults equal and 1 → Gain;
- all equal and 0 with displayMin < 0 → Offset;
- script name matching `gamma` (case-insensitive) with default 1 → Power;
- otherwise → Colour.

The role is GUI-only metadata: not serialised and not in the cache hash. Open question Q4.

### 4.2 Rim scale

The rim is a scale, not a clamp. The disc maps radius `rimScale`. When a drag pushes the puck past 90 % of the rim, the scale steps up through 0.05, 0.1, 0.25, 0.5, 1, 2, 4 and a small "rim 0.5" label updates. When an edit ends with the puck under 30 % it steps back down. Explicit +/− buttons sit beside the label. The step is chosen from the knob's value when the panel opens, so a strong existing grade is not pinned at the edge.

### 4.3 Drawing

The disc is an iso-luminant preview: each point shows what the knob at that chroma does to mid-grey, `Yp·1 + E·(chroma)` with Yp = 0.18 for ratio roles, or `0.18·1 + chroma` for offsets. It is converted through the working → `color_picking` processor (the same space the swatch uses, `Gui/KnobGuiColor.cpp:375-380`) in one `PackedImageDesc` call and cached per (size, rim, role, config). Points whose preview has a negative channel are drawn desaturated and hatched. The gamut-edge triangle, the Planckian locus arc (gain/colour roles) and the remembered-hue tick are overlaid. Being iso-luminant, the disc itself shows the value-preservation promise: blue really does look dark at the same luminance.

### 4.4 Alpha, dimensions, animation

- **Alpha is never touched** by wheel, value, temp or tint gestures. 4-dim knobs get a separate A slider row in the panel. 3-dim knobs have none. 1-dim knobs show only the value control and its field, with the wheel hidden.
- A folded knob (all dims equal) unfolds when a wheel edit makes RGB differ (`KnobGuiColor::onMustShowAllDimension`, `.cpp:334-338`). Folding never re-equalises alpha.
- **Expressions** on any RGB dimension make the panel read-only (puck shown, gestures off). A disabled knob disables the panel (`setEnabledExtraGui`, `.cpp:513-518`).
- **Animated knobs** behave like slider edits: values are written at the current frame through `KnobUndoCommand`'s `knobUI->setValue`, so autokey and keyframe refresh work exactly as they do for sliders.
- **Views:** `ViewSpec::all()`, as today.

### 4.5 Undo and render

- **One undo step per press→release gesture**, per typed field commit and per reset. Each move pushes a `KnobUndoCommand<double>` over all dims (old values = the start of the gesture), like the slider. P4.T3 adds a *merge key* (gesture serial) to `KnobUndoCommand`, so a command merges only with a command carrying the same key. Two consecutive wheel drags therefore stay two steps, which the slider path does not guarantee today. Slider behaviour is unchanged (key 0 = legacy merge).
- Draft render is on during a gesture, then turned off with a final `renderAllViewers` when moved and auto-proxy is on, as in `onSliderEditingFinished`. With `getRenderOnEditingFinishedOnly()`, the puck moves live but nothing is pushed until release, which then pushes one command.

### 4.6 Replacing the popup

The existing wheel-icon popup button becomes the **dropdown toggle** (▸/▾). The popup's useful parts move into the panel's second tab ("Values"): RGB and HSV numeric fields *without* the [0,1] clamp, hex (display-referred, clamped, labelled as such) and the 2×12 palette. The QtColorTriangle is dropped. Open question Q5.

## 5. The three pitches

All three share §3–§4. They differ in **how a gesture picks its axis**, which decides what the two usable modifiers (Ctrl, Shift) are left free for.

### Pitch A — "Zoned wheel" (Resolve-style geometry; modifiers = gearing)

The panel's geometry picks the axis: the disc interior is the trackball, the outer ring band is hue, a vertical bar is value (the master-ring analogue), and horizontal jog strips are saturation, temperature and tint. Ctrl and Shift only gear.

Collapsed (unchanged except the toggle glyph):
```
      Gain │ [ 1.000 ][━━━━━━━━●━━━━━━━━━━━━]  [4] [■] [◎▸]
```
Expanded:
```
      Gain │ [ 1.062 ][ 0.997 ][ 0.951 ][ 1.000 ]   [4] [■] [◎▾]
           │ ┌───────────────────────────────────────────────────────────┐
           │ │ [Wheel] [Values]                               rim 0.25 ± │
           │ │          .--~~ hue ring ~~--.        ┌─┐  Hue   [  18.4°] │
           │ │       .'   G         Y      '.       │ │  Sat   [  0.071] │
           │ │      /         .    .         \      │ │  Value [  1.000] │
           │ │     |  C    ·    +  ·●    R    |     │━│◀ Temp  [ +2.0 mrd]│
           │ │      \        locus·          /      │ │  Tint  [  0.00 ] │
           │ │       '.   B         M      .'       │ │  Sedge [  0.12 ] │
           │ │          '--..________..--'          └─┘  V               │
           │ │  Sat  ◀━━━━━━━━━●━━━━━━━━━━━━━▶                     [↺]   │
           │ │  Temp ◀━━━━━━━━━━━━━┃━━━━━━━━━━━━━▶  (jog, springs back) │
           │ │  Tint ◀━━━━━━━━━━━━━┃━━━━━━━━━━━━━▶  (jog, springs back) │
           │ │  A    ◀━━━━━━━━━━━━━━━━━━━━━━━●━━▶   [ 1.000]            │
           │ └───────────────────────────────────────────────────────────┘
```

Gesture map (k = gear multiplier):

| Where / what | Effect | Base rate (k = 1) |
|---|---|---|
| Press+drag in disc interior | trackball: puck moves by cursor *delta* (relative, no jump to the click point) → hue+sat | 1 px = rim/R_px chroma (R_px = disc radius); at 1× the puck tracks the cursor |
| Press+drag on outer ring band (12 px) | hue only | Δθ = swept angle about the centre (cursor tracks the ring) |
| Sat strip | saturation only (radius; from the remembered hue at zero) | 1 px = rim/R_px |
| V bar, vertical drag | value only (up = brighter) | Gain/Colour/Power: 1 px = 1/100 stop; Offset: 1 px = rim/R_px |
| Temp / Tint jog strips | temperature / tint (relative; the strip springs back, the readout is absolute) | Temp 1 px = 0.25 mired; Tint 1 px = 0.05 (×1000 Duv); Offset: 1 px = rim/R_px |
| **Ctrl** held (any of the above) | gear **×0.1** (fine; matches Natron Ctrl = finer) | |
| **Shift** held | gear **×4** (fast) | |
| **Ctrl+Shift** held | gear **×0.01** (ultra-fine; matches the slider's Ctrl+Shift ×100 zoom) | |
| Gear changed mid-drag | applies from the next move event, with deltas integrated, so there is no jump | |
| Double-click disc | reset chroma to neutral (keep value); one undo step | |
| Double-click V bar | reset value to neutral (Gain/Power 1, Offset 0; Colour: no-op) | |
| [↺] | reset all RGB to the knob's defaults (A untouched) | |
| Swatch, plain click | toggle viewer picking (unchanged) | |
| Swatch, plain drag past `startDragDistance` | *optional, Q6:* trackball on the collapsed row, no picking toggle | as disc interior, 1 px = rim/64 |

Centre/radius: §4.1. Alpha: A strip only. Undo: §4.5, one step per strip/disc/bar gesture.

*Pros.* Fully discoverable: every axis is visible and the tooltip just names the zones. Modifiers mean the same thing as on every Natron slider. Hue-only is a natural ring rotation and value is the master-ring idea the user cited. Works with a pen or touchpad (no middle button). The absolute readouts double as numeric entry. *Cons.* The largest panel (about 260 px tall) pushes knobs below it down, though it is collapsed by default and the toggle is per knob. More widgets to build and test (one wheel plus five strips). Hue-only on a small ring band needs a hit test that tolerates near-misses.

### Pitch B — "Modifier-mapped wheel" (Nuke-style; compact)

One wheel and the readouts, with no strips. The modifier held at press time picks the axis. Gearing cannot also be a modifier, so it moves to the scroll wheel.

Collapsed: as A.
Expanded:
```
      Gain │ [ 1.062 ][ 0.997 ][ 0.951 ][ 1.000 ]   [4] [■] [◎▾]
           │ ┌─────────────────────────────────────────────┐
           │ │      .--~~~~~~~~--.      Hue  [ 18.4°]      │
           │ │    .'  G       Y   '.    Sat  [ 0.071]      │
           │ │   |  C   ·  + ·●  R  |   Val  [ 1.000]      │
           │ │    '.  B       M   .'    Temp [+2.0 mrd]    │
           │ │      '--........--'      Tint [ 0.00 ]      │
           │ │  gear [1×]  rim 0.25 ±   A    [ 1.000]      │
           │ └─────────────────────────────────────────────┘
```

| Gesture | Effect | Base rate |
|---|---|---|
| Drag | trackball hue+sat (relative) | 1 px = rim/R_px |
| **Ctrl**+drag | hue only (horizontal) | 1 px = 0.5° |
| **Shift**+drag | saturation only (horizontal) | 1 px = rim/R_px |
| **Ctrl+Shift**+drag | value only (vertical, up = brighter) | 1/100 stop or rim/R_px |
| **Middle**-drag | temperature (x) + tint (y) together, a "white-balance trackball" | 0.25 mired / 0.05 per px |
| Scroll wheel *during* a drag | gear steps through ×0.01, ×0.1, ×0.25, **×1**, ×4; shown in the `gear` badge | |
| Click the `gear` badge | cycles the resting gear (session-only) | |
| Double-click | reset chroma; Ctrl+Shift+double-click resets value | |
| Swatch | plain click picks; the same modifier-drags work on the collapsed swatch | |

Centre/radius, alpha, undo: as §4. *Pros.* Compact (about 130 px). Probably close to Nuke muscle memory if Foundry's bindings are as recalled (to verify). Every axis is also reachable from the collapsed swatch. *Cons.* Gearing is not a modifier, contrary to the user's ask: scroll-during-drag works but is awkward on touchpads and pens. Ctrl means "hue" here but "finer" on every Natron slider. Middle-drag is missing on many laptops and pens, and modifier axes are invisible until read in a tooltip.

### Pitch C — "Held-key axes, swatch-first" (Blender/Houdini modal; modifiers = gearing)

The swatch itself is the trackball, so the panel is optional. Holding a letter key while dragging picks the axis, and a small HUD names it. Ctrl and Shift gear exactly as in A.

Collapsed (live trackball on the swatch):
```
      Gain │ [ 1.000 ][━━━━━━━━●━━━━━━━━━━━━]  [4] [■] [◎▸]
                                                    ↑ drag = hue+sat; hold H/S/V/T + drag
                                               ┌──────────────┐
                                               │ V  value 1.04│   ← HUD while dragging
                                               └──────────────┘
```
Expanded (small: the wheel plus readouts, the same gestures as the swatch):
```
           │ ┌──────────────────────────────────────┐
           │ │    .--~~~~--.   H [ 18.4°]  S [0.071]│
           │ │  .' G    Y  '.  V [ 1.000]  A [1.00] │
           │ │ | C  · +·●  R | T [+2.0]   Tn [0.00] │
           │ │  '. B    M  .'  rim 0.25 ±           │
           │ │    '--....--'   hold H S V T to lock │
           │ └──────────────────────────────────────┘
```

| Gesture (on swatch or wheel) | Effect | Base rate |
|---|---|---|
| Plain click (swatch) | toggle picking (unchanged); a release before `startDragDistance` counts as a click | |
| Plain drag | trackball hue+sat | swatch: 1 px = rim/64; wheel: rim/R_px |
| Hold **H** + drag | hue only (horizontal) | 1 px = 0.5° |
| Hold **S** + drag | saturation only (horizontal) | 1 px = rim/R_px |
| Hold **V** + drag | value only (vertical) | 1/100 stop or rim/R_px |
| Hold **T** + drag | temperature (x) + tint (y) | 0.25 mired / 0.05 per px |
| Ctrl / Shift / Ctrl+Shift | gear ×0.1 / ×4 / ×0.01 | |
| Esc during a drag | cancel: restore the start values, no undo step | |

*Pros.* The fastest for experts: no panel needed for most nudges, and it works across a column of colour knobs without expanding any. Modifiers keep Natron's meaning. Esc-cancel is a nice extra (A could adopt it too). *Cons.* Low discoverability. Held letter keys need keyboard focus on the swatch/wheel, so they compete with the properties panel and with application shortcuts, and auto-repeat events must be filtered. Accessibility is poor (hold a key and drag). Tablet-only users have no H/S/V/T. Most of the work is in input plumbing rather than visible UI.

## 6. Recommendation

**Pitch A**, with C's **Esc-to-cancel** and, subject to Q6, C's **plain-drag trackball on the collapsed swatch**. A is the only pitch that meets "gearing is a modifier" while keeping Natron's existing Ctrl = finer meaning. It makes every axis visible, gives value the separate ring/bar the user's references all share, and keeps the colour maths behind a gesture layer (`WheelGesture { axis, delta, gear }`) that a future hardware trackball, or B/C-style bindings, can drive without touching the maths. B's compactness is real but buys it by breaking both gearing and the Ctrl convention. C is a strong *expert addition* on top of A rather than a replacement.

Colour space: **linear luma–chroma with hexcone hue angles and working-space luma from OCIO** (§3.3). Temperature: **Planckian (CCT/Duv) for Gain/Colour/Power, linear axes for Offset** (§3.4).

## Phase 44.1: Colour maths (pure, no Qt, no OCIO)

- [ ] M44.P1.T1 — Add the luma–chroma split and hue/sat/value edits for the Gain, Offset and Colour roles
  - files: `Engine/ColorWheelMath.h` (new), `Engine/ColorWheelMath.cpp` (new), `Tests/ColorWheelMath_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: Use the §3.3 maths. A `LumaBasis` struct built from `w[3]` (renormalised to Σ = 1) holds F (fixed) and E. Stateless functions: `split(basis, rgb) → {Y, a, b}`, `join(basis, Y, a, b) → rgb`, and `polar`/`fromPolar`. `WheelState stateFor(role, basis, rgb)` returns the puck position, θ, ρ, V and S_edge for the role's normalisation (ratio: (a, b)/Y; offset: absolute). Edits are `rotateHue(role, basis, rgb, dθ)`, `setSaturation(role, basis, rgb, ρ, rememberedHue)`, `moveChroma(role, basis, rgb, da, db)`, `scaleValue`/`addValue` per §4.1, and `resetChroma`/`resetValue`. Y ≤ 1e-6 on a ratio role returns `valid=false` for the chroma edits. Values are never clamped. Doubles throughout.
  - verify: `ctest -R ColorWheelMath`. Pure R/G/B sit at θ = 0/120/240° and Y/C/M at 60/180/300°. Under AP1 and Rec.709 weights, every chroma edit keeps Y within 1e-12 relative. The §3.3 worked example matches (1.109, 1.109, 1.609) ±1e-3. split∘join round-trips 1e-12. A zero delta is the identity. HDR (gain 8) and negative (offset −0.3) inputs survive. The Y ≤ 0 ratio case reports invalid and value-add escapes it. Rotation by 360° returns the input.
  - size: M

- [ ] M44.P1.T2 — Add Planckian temperature/tint and the offset-role linear axes
  - files: `Engine/ColorWheelMath.h`, `Engine/ColorWheelMath.cpp`, `Tests/ColorWheelMath_Test.cpp`
  - approach: Use the §3.4 model. `planckianXY(T)` uses the Kim et al. cubic (1667–25000 K) and `xyToCctDuv` uses Ohno 2013 (or Robertson with its 31-entry table). `tempTint(basis, xyzMatrix, rgb, whiteRgb) → {mired, tint, valid}`. `setTempTint(…, mired, tint)` builds XYZ with Y held at w·rgb, then converts back through the inverse matrix. The offset-role `tempTintAxes(basis, xyzMatrix)` returns two unit (a, b) directions from the locus tangent/normal at white. Out-of-domain inputs return `valid=false`, and the caller switches to the linear axes.
  - verify: `ctest -R ColorWheelMath`. A neutral gain reads 0/0. Planckian xy matches published CIE values at 2856 K, 5003 K and 6504 K to 1e-4. Every temp/tint edit keeps Y within 1e-12 relative. An edit followed by its inverse returns the input to 1e-9. Offset axes are orthogonal to **1** in the split sense (w·join(0, axis) = 0).
  - size: M

- [ ] M44.P1.T3 — Add the Power role (gamma through the mid-grey effect vector) and role inference
  - files: `Engine/ColorWheelMath.h`, `Engine/ColorWheelMath.cpp`, `Tests/ColorWheelMath_Test.cpp`
  - approach: `powerToEffect(γ) = 0.18^(1/γ)` and its inverse `γ = ln 0.18 / ln e` (guard e ≤ 0 or e = 1 → invalid). Power edits wrap the Gain-role edits. Add `inferColorRole(defaults, displayMin, scriptName)` with the §4.1 rules.
  - verify: `ctest -R ColorWheelMath`. Hue/sat/temp edits on γ = (1, 1, 1.2) keep Y(0.18^(1/γ)) within 1e-12 relative. γ = 1 maps to 0.18 and back. The inference table is tested row by row: Grade's seven knobs by default+range+name, Constant's colour, and an OFX-like `gamma` param.
  - size: M

## Phase 44.2: Engine plumbing

- [ ] M44.P2.T1 — Expose the working space's luma weights and XYZ matrix from OCIO
  - files: `Engine/ProjectColorManagement.h`, `Engine/ProjectColorManagement.cpp`, `Tests/ProjectColorManagement_Test.cpp` (create it if no OCIO test file exists; check `Tests/` first)
  - approach: Add `bool getWorkingToXYZ(double m[9]) const` and `void getWorkingLuma(double w[3]) const`. Probe `getConversionProcessor(working, "cie_xyz_d65_interchange")` (via `resolveRoleOrName`) with unit vectors. Check linearity (additivity and 2× scaling to 1e-5). Fall back to `config->getDefaultLumaCoefs()`, then Rec.709. Renormalise w. Cache under `_mutex` and invalidate in `setWorkingSpace` and on config change, next to `rebuildPickingProcessors`.
  - verify: `ctest -R ProjectColorManagement`. On the built-in ACES 2.0 Studio config with ACEScg working, w matches ColorMath's AP1 weights (`Engine/Nodes/Image/ColorMath.h`) to 1e-4. Switching the working space to a lin_rec709 space gives Rec.709 weights. A log working space reports a non-linear probe and Rec.709 fallback weights.
  - size: M

- [ ] M44.P2.T2 — Add `KnobColor` roles and declare them on the native colour nodes
  - files: `Engine/KnobTypes.h`, `Engine/KnobTypes.cpp`, `Engine/Nodes/Color/Grade.cpp`, `Engine/Nodes/Color/ColorCorrect.cpp`
  - approach: Add `ColorKnobRoleEnum { eColorKnobRoleAuto, Gain, Offset, Colour, Power }` and `setColorRole`/`getColorRole`, where Auto resolves through `inferColorRole` from the knob's dimension-0 defaults, display minimum and script name. GUI metadata only: no serialisation, no hash. Pass explicit roles from `addGradeColorKnob`/`addScaleKnob` (new parameter): blackPoint/black/offset → Offset; whitePoint/white/multiply, plus ColorCorrect saturation/contrast/gain → Gain; gamma → Power. Constant, CheckerBoard and the keyers stay on Auto, which infers Colour. List them in the PR.
  - verify: `ctest -R "Grade|ColorCorrect|ColorWheelMath"`. A new test case in the Grade test file (or `ColorWheelMath_Test.cpp` if Grade has none) creates a Grade and checks the seven roles. Existing Grade/ColorCorrect render tests are unchanged.
  - size: M

## Phase 44.3: The wheel widget (standalone)

- [ ] M44.P3.T1 — Build `ColorWheelWidget` painting: iso-luminant disc, ring, puck, gamut-edge triangle, locus arc, rim label
  - files: `Gui/ColorWheelWidget.h` (new), `Gui/ColorWheelWidget.cpp` (new), `Tests/ColorWheelWidget_Test.cpp` (new), `Tests/CMakeLists.txt` (GuiTests list)
  - approach: A QWidget with no knob knowledge. `setBasis(LumaBasis, xyz)`, `setRole`, `setState(WheelState)`, `setRimScale`, and `setDisplayConverter(std::function<void(float*, int)>)` for the bulk working → colour_picking conversion (§4.3). The disc QImage is cached per (size, rim, role, basis hash) and redrawn on invalidation. Hue positions follow §3.3 (red at 3 o'clock, counter-clockwise). Rim auto-stepping per §4.2 is a pure helper here.
  - verify: `GuiTests --gtest_filter=ColorWheelWidget*`. Rendered offscreen at 160 px, the puck is drawn at the pixel `stateFor` predicts (±1 px). The disc centre pixel is achromatic (|r−g|, |g−b| < 2/255) under an identity converter. Rim stepping goes up at 90 % and down at 30 %. One `grab()` PNG is written to the test output directory for eyeballing.
  - size: M

- [ ] M44.P3.T2 — Add wheel gestures: zone hit-testing, relative trackball, ring hue, gearing, double-click resets, Esc cancel
  - files: `Gui/ColorWheelWidget.h`, `Gui/ColorWheelWidget.cpp`, `Tests/ColorWheelWidget_Test.cpp`
  - approach: Hit-test press into disc / ring band (12 px, with 4 px tolerance). The trackball uses cursor deltas, and the ring uses the swept angle. Read the gear from modifiers on every move (Ctrl ×0.1, Shift ×4, Ctrl+Shift ×0.01) and integrate deltas. Emit `gestureStarted()`, `gestureMoved(WheelEdit)` (a value type holding axis and amount, not RGB, so the widget stays maths-agnostic) and `gestureFinished(bool moved)` / `gestureCancelled()`. Double-click emits `resetChroma`. Never react to Alt or Meta.
  - verify: `GuiTests --gtest_filter=ColorWheelWidget*`. Synthetic QTest mouse events check that a 50 px interior drag emits a chroma delta of 50·rim/R_px. The same drag with Ctrl gives ×0.1 and with Shift ×4. A ring drag through 90° emits dθ = 90° ± 1°. Pressing Ctrl halfway through a drag causes no jump. A double-click emits reset. Esc emits cancel.
  - size: M

- [ ] M44.P3.T3 — Build `ColorAdjustPanel`: wheel + V bar + Sat strip + Temp/Tint jog strips + A strip + readout fields + Values tab
  - files: `Gui/ColorAdjustPanel.h` (new), `Gui/ColorAdjustPanel.cpp` (new), `Tests/ColorAdjustPanel_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: Lay out §5A. The strips are a small `JogStrip` class in the same file (absolute mode for Sat/A, spring-back mode for Temp/Tint), with the same gear rules. The panel owns the maths calls: `setKnobValues(rgba, nDims, role)` in, `valuesEdited(rgba, GesturePhase)` out, where `GesturePhase` is Start/Move/End/Cancel. Typed readout fields commit as Start+End. The Values tab holds unclamped RGB/HSV fields, the hex field (clamped, labelled display-referred) and the palette (reuse `ColorSelectorPaletteButton` from `Gui/ColorSelectorWidget.h:52`). 1-dim knobs hide the wheel and strips except V. 3-dim knobs hide A.
  - verify: `GuiTests --gtest_filter=ColorAdjustPanel*`. Driving the V bar on a Gain knob at (1.2, 1, 0.8, 1) multiplies RGB by 2^(Δ/100) and leaves A at 1. A Temp jog keeps w·rgb within 1e-9 relative. Sat at zero chroma grows from the remembered hue. Typing Hue = 120 on (1.5, 1, 1) yields equal-Y green. A typed HDR value of 4.0 in the Values tab round-trips as 4.0, not 1.0.
  - size: L

## Phase 44.4: Knob integration

- [ ] M44.P4.T1 — Add a below-row widget hook to `KnobGuiValue` and top-align labels of rows that use it
  - files: `Gui/KnobGuiValue.h`, `Gui/KnobGuiValue.cpp`, `Gui/KnobGui.h`, `Gui/KnobGuiContainerHelper.cpp`
  - approach: Add a protected virtual `QWidget* createBelowRowWidget(QWidget* parent)` (default null). When it is non-null, `createWidget` wraps `_imp->container` and that widget in a VBox column widget added to the field layout, hidden by default. Add `virtual bool wantsTopAlignedLabel() const` (default false); `KnobGuiContainerHelper` ORs `Qt::AlignTop` into `labelAlignment` when it is true (`:840-860`). Existing knobs are untouched because the defaults change nothing.
  - verify: the existing `GuiTests` suite and `ctest` are green. A GuiTests case with a test-only `KnobGuiValue` subclass that returns a 100 px widget shows the row growing when the widget is shown, with the label's y unchanged (top-aligned).
  - size: M

- [ ] M44.P4.T2 — Wire `ColorAdjustPanel` into `KnobGuiColor` as the dropdown, replacing the popup
  - files: `Gui/KnobGuiColor.h`, `Gui/KnobGuiColor.cpp`
  - approach: Make `_colorSelectorButton` a checkable toggle (▸/▾ glyph on the wheel icon) and drop the `QWidgetAction` popup for non-simplified knobs (the simplified path keeps it, §0). Create the panel through `createBelowRowWidget`. Feed it the knob values, role, basis and display converter from P2.T1 (via the project's `ProjectColorManagement`, as `updateLabel` does at `:369-380`). Refresh on `updateExtraGui` and on config change. Map `valuesEdited` to the P4.T3 undo path. Disable on expressions or a disabled knob, and unfold dims when RGB stops being equal. Expanded state is per KnobGui instance and not persisted.
  - verify: `GuiTests` case: build a Grade's settings panel offscreen and toggle `white`'s dropdown. The panel shows with Gain role and the puck at the centre. Toggling again hides it. The swatch plain click still emits `pickingEnabled(true)`.
  - size: M

- [ ] M44.P4.T3 — Make each gesture one undo step via a gesture-scoped merge key on `KnobUndoCommand`
  - files: `Gui/KnobUndoCommand.h`, `Gui/KnobGuiColor.cpp`, `Tests/ColorAdjustPanel_Test.cpp` (or a new `Tests/KnobGuiColorUndo_Test.cpp` in GuiTests)
  - approach: Add an optional `quint64 mergeKey` (0 = today's behaviour) to the list constructor. `mergeWith` additionally requires equal keys when either key is non-zero. In `KnobGuiColor`: on Start, take a new key from a static counter and record the start RGBA; on each Move, push `KnobUndoCommand<double>(start, current, …)` with the key, with draft render on; on End, turn draft off and render as `onSliderEditingFinished` does; on Cancel, push nothing and restore the start values silently. Honour `getRenderOnEditingFinishedOnly()`: no pushes on Move, one push on End.
  - verify: GuiTests: two consecutive drags on Grade `white` give exactly two undo steps. Undo restores the pre-drag RGBA bit-for-bit. Under autokey, a drag at frame 5 creates one key, and undo removes it. Esc-cancel leaves the undo stack unchanged. Existing slider undo tests are unchanged.
  - size: M

- [ ] M44.P4.T4 — Plain-drag trackball on the collapsed swatch (if Q6 = yes)
  - files: `Gui/KnobGuiColor.h`, `Gui/KnobGuiColor.cpp`
  - approach: Move `ColorPickerLabel`'s toggle from press to release when the pointer moved less than `QApplication::startDragDistance()`. Past it, run a trackball gesture (1 px = rim/64, same gears, Esc cancel) through the same Start/Move/End path, even when the panel is collapsed. Update the swatch tooltip to name the drag and the gears.
  - verify: GuiTests: a click (press+release, 0 px) toggles picking. A 30 px drag changes chroma, does not toggle picking and gives one undo step. A 2 px jitter still counts as a click.
  - size: M

## Phase 44.5: GUI evidence

- [ ] M44.P5.T1 — Add a GUI script that exercises the dropdown on Grade and ColorCorrect and captures shots
  - files: `Tests/gui/m44_trackball.py` (new)
  - approach: Use the `guitest.py` helpers. Graph: Constant (grey 0.18) → Grade → Viewer. Expand Grade `white`. Drive the wheel via the widget's object names using synthetic QTest-style events through PySide (or the Natron Python knob API for the assertions). Shots: collapsed row, expanded panel at neutral, mid-trackball drag, ring hue drag, V bar, Temp jog, an Offset (`black`) knob with a non-zero chroma, and a 1-dim knob. Checks: the viewer centre colour (`viewer_centre_colour()`) changes hue while luminance under the working weights stays within 1 %, and one `app.undo` restores the original values.
  - verify: `Tests/gui/run-gui-test.sh Tests/gui/m44_trackball.py` exits 0. The shots in `build/gui-test-out/` show the panel at each step.
  - size: M

**Verification gate:** CI `format`, `lint-ci` and `build-and-test` green, including the new `ColorWheelMath`, `ProjectColorManagement`, `ColorWheelWidget`, `ColorAdjustPanel` and undo cases. `Tests/gui/run-gui-test.sh Tests/gui/m44_trackball.py` passes, and its screenshots (collapsed, expanded neutral, trackball, hue ring, value bar, temperature, offset knob, 1-dim knob) are attached to the PR.

## 7. Risks

- **Label/field layout.** Inline growth relies on P4.T1's VBox wrap and top-aligned label. Rows where several knobs share a line (`getKnobsCountOnSameLine() > 1`, `Gui/KnobGuiValue.cpp:230`) would grow only their own cell. Mitigation: in that case the toggle opens the panel as a popup (the old `QWidgetAction` path), noted in the PR.
- **OCIO role absence.** A custom config without `cie_xyz_d65_interchange` falls back to the config's luma coefficients. Planckian temperature then needs an XYZ matrix, so those configs get the linear axes only, with the Temp readout showing "—".
- **Planck inversion edge cases.** These are contained in P1.T2 and test-covered. Fallbacks are explicit `valid=false` results, never NaNs reaching a knob.

## 8. Open questions

1. **Pitch.** (a) A, zoned wheel, modifiers = gearing. (b) B, modifier-mapped, Nuke-like, gearing on the scroll wheel. (c) C, held-key axes, swatch-first. **Recommended: (a)**, plus C's Esc-cancel.
2. **Colour model.** (a) Linear luma–chroma, hexcone hue angles, working-space luma from OCIO, exact luminance preservation. (b) OKLCh with a Y-renormalise step: nicer hue spacing, but non-linear, and the knob's meaning drifts. (c) HSV, as in Nuke. **Recommended: (a).** OKLab could later warp the *drawn* hue spacing only.
3. **Temperature model.** (a) Planckian CCT/Duv (mired, with Duv-based tint) for gain/colour/gamma knobs, linear axes for offsets. (b) One fixed linear blue–amber and green–magenta axis pair for every knob (simpler; drifts green when warming, like Nuke's TMI). **Recommended: (a).**
4. **Knob roles.** (a) Explicit roles on native nodes plus inference (defaults/display-min/`gamma` name) for OFX and Python knobs. (b) Inference only. (c) Also expose `setColorRole` to Python/PyPlug authors. **Recommended: (a)**; (c) can follow if anyone asks.
5. **The old popup.** (a) Replace it: the dropdown's "Values" tab keeps unclamped RGB/HSV, hex and the palette, and the QtColorTriangle is dropped. (b) Keep both buttons. **Recommended: (a).** The popup also clamps HDR to [0,1] and records no undo (§1), so keeping it keeps two bugs.
6. **Collapsed-swatch drag.** (a) Yes: a plain drag on the swatch is a trackball nudge, and a click still picks (P4.T4). (b) No: the swatch stays click-only and all editing happens in the panel. **Recommended: (a).** It is the quickest nudge and costs one task.
