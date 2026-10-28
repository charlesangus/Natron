# M95 - Knob Cleanup And Keyer Redesign

Split from M69 - Minor Cleanup on 2026-10-11. It removes the per-node `hidpi` and `interactive` knobs in favour of app-level DPI (already automatic) and one global interactive toggle with a shortcut. It moves clamp black/white below the line, above "(Un)premult by", and gives OFX nodes the same footer. M137 - Native Base Class Flexibility places the native stage knobs: separator, (Un)premult by, mask, Invert mask and Mix. It redoes the native Keyer (M67.P5.T1) as a four-value trapezoid matte that outputs alpha only.

Proposed Deps: M69 (both edit `ActionShortcuts.h`, `GuiApplicationManager10.cpp`, `ViewerTab.cpp`), M76 (both edit `ExtentKnobs.*`), M137 - Native Base Class Flexibility (the stage-knob footer and the framework Keyer is rewritten on).

Scouting (2026-10-11, `10e0accd5`):
- Native `interactive` knobs: `ExtentKnobs.cpp:242-258` (Constant, CheckerBoard, Crop), `Transform.cpp:393-409`, `Position.cpp:229-234`, Roto `kRotoTransformInteractive` (`RotoContextPrivate.h:332-334,1994-2001`).
- Every host overlay already falls back to `!Settings::getRenderOnEditingFinishedOnly()` when its `interactive` knob is null (`Gui/HostOverlay.cpp:300-305,733-738,941-946,1585-1590`). The Tracker already passes null.
- `hidpi` is read by no native code. Native overlays scale by `getScreenPixelRatio()`/`TO_DPIX`, so DPI is already automatic.
- OFX plugins from openfx-supportext still declare `hidpi` (`ofxsOGLHiDPI.h`) and `interactive`/`transformInteractive`/`rampInteractive`: CornerPin, Card3D, Ramp, Radial, Rectangle, GodRays, HSVTool, ImageStatistics, the OIIO text and SeExpr plugins, among others. `tools/bench/comps/*.ntp` carry `hidpi`.
- Loading a project skips serialized knobs that no longer exist, silently (`Node::loadKnob` matches live knobs by name, `Engine/Node.cpp:1724`). Per M67's clean-break rule, no migration code is written.
- M93 overlap: M93's golden-XML harness loads `tools/bench/comps/*.ntp`, which contain `hidpi`. Whichever of M93/M95 lands second regenerates `Tests/fixtures/golden-ntp/`.

## Phase 95.1: Global interactive toggle

- [ ] M95.P1.T1 — Expose the global "interactive updates" toggle as an action, a shortcut and a viewer button
  - files: `Gui/ActionShortcuts.h`, `Gui/GuiApplicationManager10.cpp`, `Gui/ViewerTab.cpp`, `Gui/ViewerTabPrivate.h`, `Engine/Settings.cpp`
  - approach:
    - The toggle *is* the existing preference `renderOnEditingFinished` (`Settings.cpp:652-656`), inverted. Don't add a second setting.
    - Add a global keybind `kShortcutIDActionToggleInteractive` (Q1 default: Alt+U, free in every group). Add a checkable "Interactive" button beside `turboButton` (`ViewerTab.cpp:816`) in every viewer. Both call `Settings::setRenderOnEditingFinishedOnly(!x)` (`:3228`) and save settings.
    - `Settings::onKnobValueChanged` for that knob tells every `ViewerTab` to sync its button, so the Preferences checkbox, the shortcut and the button agree.
  - verify: P5.T1: the shortcut flips the button in two viewers and the Preferences checkbox. With it off, dragging a Transform's overlay renders only on release; with it on, renders during the drag.
  - size: M

## Phase 95.2: Remove per-node `hidpi`/`interactive`

- [ ] M95.P2.T1 — Remove `interactive` and `hidpi` from the generator/extent knobs
  - files: `Engine/Nodes/Image/ExtentKnobs.h`, `Engine/Nodes/Image/ExtentKnobs.cpp`, `Tests/Native/NativeConstant_Test.cpp`, `Tests/Native/NativeCrop_Test.cpp`
  - approach:
    - Delete the two knobs (`ExtentKnobs.cpp:242-258`), `_interactive`/`_hiDPI` (`.h:160-161`), their `#define`s (`.h:42-43`) and their secret toggling (`:313-314`).
    - Pass a null `KnobBoolPtr` to `addRectangleInteract` (`:284-287`); the overlay then uses the global toggle.
    - Update the test assertion at `NativeConstant_Test.cpp:301` to expect a null interactive slot, and drop any knob-list expectations naming them.
    - **Coordinate with M76**, which reuses `ExtentKnobs` for NativeWrite (`M76` P1 task "reuse the format knobs as `ExtentKnobs.cpp:159` builds them"). Rebase onto M76 if it has merged.
  - verify: compiled at the gate. `ctest -R "NativeConstant|NativeCrop|RectangleOverlay"` passes. A Constant/CheckerBoard/Crop panel shows neither knob.
  - size: M

- [ ] M95.P2.T2 — Remove `interactive`/`hidpi` from Transform, Position and the Roto transform page
  - files: `Engine/Nodes/Transform/Transform.h`, `Engine/Nodes/Transform/Transform.cpp`, `Engine/Nodes/Transform/Position.h`, `Engine/Nodes/Transform/Position.cpp`, `Engine/RotoContextPrivate.h`
  - approach:
    - Delete `kTransformNodeParamInteractive`/`HiDPI` and their knobs (`Transform.cpp:393-409`, `.h:58-59,213-214`), and pass null at `addTransformInteract` (`:527`).
    - Delete `kPositionParamInteractive` and its knob (`Position.cpp:229-234`); check whether it was ever passed to `addPositionInteract` and pass null if so.
    - Delete the Roto `transformInteractive` knob (`RotoContextPrivate.h:1994-2001`, `transformInteractiveKnob` `:1364`) and pass null at `:2027`. Remove its defines `:332-334`.
    - Grep `Tests/Native/NativeTransform_Test.cpp` and `NativePosition_Test.cpp` for the names and update them.
  - verify: compiled at the gate. `ctest -R "NativeTransform|NativePosition|Roto"` passes. P5.T1: the Transform and RotoPaint transform pages have no Interactive/HiDPI rows, and dragging obeys the global toggle.
  - size: M

- [ ] M95.P2.T3 — Hide and host-drive OFX plugins' `hidpi`/`interactive` params (if Q3 = hide)
  - files: `Engine/Node.cpp`, `Engine/Node.h`, `Engine/AppManager.h`, `Gui/GuiApplicationManager.cpp`, `Engine/Settings.cpp`
  - approach:
    - Beside `createUnPremultSelector` (`Node.cpp:2659-2700`, the precedent for hiding a plug-in param), add `adoptHostDrivenOverlayParams()`. For a bool OFX param named `hidpi`, `interactive`, `transformInteractive` or `rampInteractive`, it calls `setSecret(true)`, `setSecretLocked(true)` and `setIsPersistent(false)`.
    - It sets the interactive ones to `!getRenderOnEditingFinishedOnly()`. It sets `hidpi` to `appPTR->getScreenDevicePixelRatio() >= 1.5`: a new virtual on `AppManager` returning 1 headless, overridden in `GuiApplicationManager` as `qApp->devicePixelRatio()`.
    - When the toggle changes (`Settings::onKnobValueChanged`), walk every project's nodes recursively and re-set the interactive params.
  - verify: P5.T1: a CornerPin and a Ramp show no HiDPI/Interactive rows, and a saved project contains neither param. Under `QT_SCALE_FACTOR=2` the CornerPin handles are the same on-screen size as Transform's.
  - size: L

## Phase 95.3: Footer below the line

- [ ] M95.P3.T1 — Move clamp black/white below the line, and give OFX nodes the same footer
  - note (2026-10-19): M137 - Native Base Class Flexibility now builds the native stage-knob footer (separator → (Un)premult by → mask → Invert mask → Mix). This task adds the clamp row above "(Un)premult by" and applies the footer order to OFX nodes through `Node.cpp`. Re-scout the lines below against what M137 merged.
  - files: `Engine/Node.cpp`, `Tests/KnobFooterOrder_Test.cpp` (new), `Tests/CMakeLists.txt` (**OVERLAP: Tests/CMakeLists.txt**)
  - approach:
    - Today `createUnPremultSelector` adds `hostUnPremultBy` at the end of the plug-in knobs (`Node.cpp:2696`), above `advancedSep`. The separator is only inserted when a mask selector exists (`:3187-3209`).
    - Add `clampBlack` and `clampWhite` to `foundPluginDefaultKnobsToReorder` (`:3130-3133`), so native and OFX nodes are both covered.
    - Build the footer as [separator] → (Un)premult by → Clamp black → Clamp white → mask channel(s) → Invert mask → Mix (Q11). Always insert the separator when any footer knob is present.
  - verify: `ctest -R KnobFooterOrder`: for native Grade, Saturation and ColorLookup, and an OFX colour plug-in with a premult pair, the main page's knob order after the last plug-in knob matches that sequence, with exactly one separator before it.
  - size: M

## Phase 95.4: Keyer redesign

- [ ] M95.P4.T1 — Add a shared trapezoid matte helper with unit tests
  - files: `Engine/Nodes/Keyer/KeyerMatte.h` (new), `Tests/Native/KeyerMatte_Test.cpp` (new)
  - approach: `inline float trapezoidMatte(double k, double a, double b, double c, double d)` returns 0 for k≤a or k≥d, 1 for b≤k≤c, and linear ramps between. Degenerate edges (a==b, c==d) are hard steps. It assumes a≤b≤c≤d, and `sortedTrapezoid` enforces that.
  - verify: `ctest -R KeyerMatte`: ramps, plateaus, hard edges, k at each breakpoint, and an all-equal trapezoid.
  - size: S

- [ ] M95.P4.T2 — Rewrite the native Keyer as a four-value matte that outputs alpha only
  - files: `Engine/Nodes/Keyer/Keyer.h`, `Engine/Nodes/Keyer/Keyer.cpp`
  - approach:
    - Knobs (Q12 defaults): key source `Luminance` (with the existing `luminanceMath`) or `Colour` (dot with `keyColor`, today's `eKeyerModeColor` maths, `Keyer.cpp:~185`). Then the four values A, B, C, D, defaulting to 0, 0.1, 0.9, 1. Delete the despill, despill angle, show, source-alpha, softness/tolerance/centre, Screen and None modes, and the `Bg`, `InM` and `OutM` inputs. The host mask/mix footer stays.
    - `knobChanged` keeps A≤B≤C≤D by pushing neighbours.
    - Render: RGB passes through untouched, and alpha = `trapezoidMatte(K, A, B, C, D)`. No premultiply.
    - Plugin identity per Q13 (default: keep `net.sf.openfx.KeyerPlugin`, bump the major to 3, no migration).
  - verify: compiled at the gate. P4.T3's tests.
  - size: L

- [ ] M95.P4.T3 — Replace the Keyer parity tests with analytic tests
  - files: `Tests/Native/NativeKeyer_Test.cpp`
  - approach: drop the OFX-parity cases. Add: a luminance ramp image whose alpha matches `trapezoidMatte` per pixel; RGB unchanged; Colour mode with a green key; the A≤B≤C≤D enforcement when B is dragged below A; and the host mask footer limiting the effect.
  - verify: `ctest -R NativeKeyer` passes at the gate.
  - size: M

- [ ] ~~M95.P4.T4 — (only if Q14 = share) Move ChromaKeyer's output onto the shared matte stage~~ (cancelled 2026-10-11: user keeps ChromaKeyer as is)
  - files: `Engine/Nodes/Keyer/ChromaKeyer.h`, `Engine/Nodes/Keyer/ChromaKeyer.cpp`, `Tests/Native/NativeChromaKeyer_Test.cpp`
  - approach: keep ChromaKeyer's angle-based key value (acceptance/suppression angle, key lift/gain), feed it through `trapezoidMatte`, and drop its show/composite/despill outputs and `Bg` input, mirroring P4.T2.
  - verify: `ctest -R NativeChromaKeyer` with rewritten analytic cases.
  - size: L

- [ ] M95.P4.T5 — Make ChromaKeyer scene-linear: no Rec.709 encoding, no clamps, base mask/mix
  - files: `Engine/Nodes/Keyer/ChromaKeyer.h`, `Engine/Nodes/Keyer/ChromaKeyer.cpp`, `Tests/Native/NativeChromaKeyer_Test.cpp`
  - approach:
    - Key in the scene-linear working space. Delete the Rec.709 transfer encoding and its `linearProcessing` toggle (`ChromaKeyer.cpp:169-171, 228-231, 702`).
    - Remove the ±0.5 Cb/Cr and [0,1] RGB clamps after suppression, and the background-key clamp (`:272-288, 302`).
    - Replace the CCIR 601/709/2020 `colorspace` choice (`:694-697`) with luma/chroma weights derived from the project's OCIO working space (as Saturation does for AP1). Give Keyer's `luminanceMath` (P4.T2) the same working-space default.
    - Replace the own inside/outside-mask clamp and blend (`:211-212`) with the host mask/mix footer, as P4.T2 does for Keyer.
    - Keep ChromaKeyer's angle-based key (acceptance/suppression angle, key lift/gain) and its outputs. This is not the alpha-only redesign that P4.T4 proposed and the user declined.
  - verify: `ctest -R NativeChromaKeyer` with analytic cases: a green key on ACEScg values above 1 and below 0 keeps the foreground colour unclipped, mask/mix through the footer, and AP1 weights under an ACES config.
  - size: L

## Phase 95.5: Evidence and UAT

- [ ] M95.P5.T1 — GUI script, shots and AppImage checkpoint
  - files: `Tests/gui/m95_knobs_keyer.py` (new), `build/appimages/M95-<sha>.AppImage`, `build/appimages/M95-uat.md`
  - approach: one section per verify above (toggle sync, Transform drag on and off, panels without the knobs, footer order shots for Grade/Saturation/ColorLookup and an OFX node, Keyer matte on a ramp and on a greenscreen still). Then `tools/ci/local/package.sh fast`. The UAT also loads `tools/bench/comps/cg_multipass.ntp` and an old project with a Keyer, to see the clean-break behaviour.
  - verify: the script reports 0 failures, and the user approves the shots and signs off on the AppImage.
  - size: L

**Verification gate:**
- One `fast` build with CI `format`, `lint-ci` and `build-and-test` green.
- ctest green, including `KeyerMatte`, `NativeKeyer`, `NativeChromaKeyer`, `KnobFooterOrder`, `NativeConstant`, `NativeCrop`, `NativeTransform`, `NativePosition` and `RectangleOverlay`, plus M93's `GoldenXml` if it has merged (regenerate its fixtures for the dropped knobs, and record that here).
- `m95_knobs_keyer.py` reports 0 failures; the user approves the shots and signs off on `build/appimages/M95-<sha>.AppImage`.

Acceptance sketch:
- Keyer has four matte values and outputs an alpha only.
- ChromaKeyer keys in scene-linear space with no clamps and uses the host mask/mix footer.
- No node has `hidpi` or `interactive` knobs; DPI is app-level and interactive is one global toggle with a shortcut.
- "(Un)premult by" and clamp black/white sit below the line with mask and mix.

## Decisions

- 2026-10-28: All phases implemented (build pending; goldens to re-record). Interactive Updates is a checkable global Render-menu action (Alt+U) synced via Settings::settingChanged; changing it saves the preference immediately. interactive/hidpi knobs removed from native generator/extent and transform nodes; OFX plug-ins' hidpi/interactive params are hidden and host-set. Footer-order test at Tests/Native/KnobFooterOrder_Test.cpp. Keyer and ChromaKeyer move to major 3. ChromaKeyer: fgy<0 zeroing removed (no clamps); background key bounded with boundForKeying (overflow guard, not a clamp) on CPU and GPU; sourceHasAlpha flag; "Add to Inside Mask" → "Inside Mask"; labels match script names. Keyer luminance math defaults to the working space. Keyer sublabel stays a hidden knob until M111's getSublabel() hook lands.
- 2026-10-19 — **Native stage-knob placement moved to M137 - Native Base Class Flexibility; Keyer is redesigned on its framework** (user, M137 grill): the framework creates the premult, mask and mix knobs, so it places them. M137 migrates Keyer and ChromaKeyer unchanged first. This milestone gains a dep on M137.
- 2026-10-11 — **Split from M69 - Minor Cleanup (user-approved).** Deps `M69, M76`.
- 2026-10-11 — **User answers** (`PLAN/DESIGN/2026-10-11-m69-questions.md`):
  - **Interactive toggle: shortcut + the existing preference only, no viewer button.** P1.T1 drops the viewer-toolbar button (`ViewerTab.cpp`, `ViewerTabPrivate.h` leave its files list); the Alt+U shortcut and the Preferences checkbox stay in sync.
  - **DPI:** automatic only; nothing to build.
  - **OFX `hidpi`/`interactive` params:** the host hides and drives them (P2.T3).
  - **Keyer:** key source Luminance or Colour, host mask/mix only (no Bg/InM/OutM); same plugin ID, major 3, clean break with no migration.
  - **ChromaKeyer:** left as is; P4.T4 cancelled.
  - **Footer order (user, from mockups at claude.ai artifact `WTqyJm8s3WDyUBadL1h8MK`):** below the separator: Clamp black and Clamp white **side by side on one row**, then (Un)premult by, then mask channel + Invert mask, then Mix. This overrides the order in P3.T1's approach (clamp black needs no new line so clamp white joins its row); the `KnobFooterOrder` test checks this order.
- 2026-10-12 — **ChromaKeyer added back (user), as a scene-linear fix rather than the alpha-only redesign:** P4.T5 removes the Rec.709 encoding and the HDR clamps and moves it to the host mask/mix (`DECISIONS/2026-10-12-native-nodes-drop-ofx-legacy.md`). The Keyer's "same plugin ID, major 3" answer is superseded by the plain-ID rule: M107 - Native Node Identity Cleanup renames it to `Keyer` at major 1.
