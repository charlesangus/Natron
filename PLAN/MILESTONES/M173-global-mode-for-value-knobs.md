# M173 - Global Mode For Value Knobs

Follow-on to M119 - Trackball Panel Layout. Global mode (Ctrl+E: click-drag strokes, gearing, Esc/Enter/right-click to exit, Ctrl+Up/Down to step knobs, one undo step per stroke, edge warp, the crosshair/reveal and widget-cursor feedback M119 keeps) works only on colour knobs today: `Gui/ColorTrackballGlobalMode.{h,cpp}` targets `KnobGuiColor*`, its stroke maths is `ColorGlobalModeStroke`, and `orderedColorKnobs` (`Gui/ColorTrackballTarget.cpp`) steps colour knobs only. This milestone extracts a kind-agnostic target interface, classifies every double/int knob into a kind (translate, rotate, scale, size, skew, generic), gives each kind its stroke maths, keys and reveal glyph, and adds a transform-group mode on nodes with a transform interact, where Q/W/E/R/T switch translate/rotate/scale/skew/center.

Code facts the plan relies on (scouted on `main`, 2026-10-24):
- Natron has no transform2d knob type. `Engine/OfxParamInstance.cpp` reads the OFX double type only at creation (1D ~948-985, 2D ~2400-2465) and keeps only `setSpatial` (XY types), `setValueIsNormalized`, `disableSlider` and `setCanAutoFoldDimensions` (Scale/XY); Angle and Scale are not retained.
- Native Transform (`Engine/Nodes/Transform/Transform.{h,cpp}`, `kTransformNodeParam*`): `translate` 2D spatial, `rotate` 1D degrees, `scale` 2D auto-fold, `skewX`/`skewY` 1D, `center` 2D. It registers `Node::addTransformInteract(translate, scale, uniform, rotate, skewX, skewY, skewOrder, center, invert, interactive)` (`Engine/Node.h:1525`, `Engine/NodeOverlay.cpp:350`); Roto does too (`RotoContextPrivate.h:1736`).
- Native Blur `size` (`Engine/Nodes/Filter/Blur.cpp:344-361`): 2D, spatial, auto-fold, increment 0.1.
- `Gui/ColorKnobEditSession.cpp:53-59` skips expression- and link-driven dimensions; values go through `KnobUndoCommand`, which keys animated knobs at the current frame.
- M119's feedback widgets (crosshair + reveal disc, widget cursor, split HUD, `ColorModeDiscWidget`) land with M119 - Trackball Panel Layout; this milestone builds on them, so re-scout file names at promotion (§5a).

## Scope

**In:**
- Global mode on every `KnobDouble` and `KnobInt` (colour keeps its own kind and behaviour unchanged).
- A value-role hint on numeric knobs (`translate`, `rotate`, `scale`, `size`, `skew`, `none`), set by native nodes, by the OFX double type at creation (Angle → rotate, Scale → scale, XY/XYAbsolute → translate), and by the knobs a node passes to `addTransformInteract`. Knobs with no hint fall back to a name/label heuristic (`translate`/`position`/`offset`…, `rotate`/`angle`, `scale`, `size`, `skew`).
- Kinds and gestures, all with the colour gearing (Ctrl ×0.1, Shift ×4, Ctrl+Shift ×0.01):
  - **Translate:** x/y drag → x/y, 1 image px per screen px.
  - **Rotate:** linear drag, right/up = CCW, left/down = CW, about 0.25°/px.
  - **Scale:** uniform by default, multiplicative, about 0.5%/px; folded knobs stay folded.
  - **Size:** uniform by default, additive at the knob's increment per px.
  - **Skew:** x → skewX, y → skewY across the two knobs, one undo step per stroke.
  - **Generic:** a 2D spatial knob (e.g. `center`) behaves and looks like translate; any other knob changes all dimensions together with either axis, at its increment per px.
- Keys on a lone knob: Q/W/E/R lock to dimension 1–4, A = all together (default).
- **Transform group:** a node that registered a transform interact. Engaging any of its interact knobs, or Ctrl+E landing on such a node, enters the group: Q translate, W rotate, E scale, R skew, T center; 1/2 lock to x/y, A = both (default).
- Engage: non-colour knobs get **no button**; Ctrl+E only. Ctrl+E targets the hovered numeric knob; else the last-touched eligible knob (any kind) on the top node; else today's colour fallback; else the node's first classified knob.
- Ctrl+Up/Down steps through every eligible knob (colour and non-colour) in panel order; the HUD and glyph switch kind as it goes.
- Per-kind reveal glyphs, widget cursor and HUD legend (knob name, active keys, gearing), designed as a Qt mockup the user approves before implementation. Starting sketches: rotate — CCW arc top-right, CW arc bottom-left, where the colour ring would be; translate — cardinal arrows; scale — scale-up icon top-right; size — large square top-right, small bottom-left; skew — a shear glyph; center — translate's arrows.

**Out:**
- Bool, choice, string and parametric knobs — global mode stays numeric. None.
- Viewer-side gestures or new overlay handles; the existing transform interact only redraws as the knobs change. None.
- An engage button on non-colour knob rows. None.

**Edge cases:**
- Expression- or link-driven dimensions are skipped, as `ColorKnobEditSession` does; a knob with every dimension driven is not eligible.
- Animated knobs key at the current frame through `KnobUndoCommand`, as colour knobs do.
- Int knobs accumulate fractional travel and step by whole units.
- A dimension lock beyond the knob's dimension count (e.g. E on a 2D knob) is ignored.
- Disabled and secret knobs are skipped when stepping, as today.
- A transform-group key whose knob the node lacks or has hidden does nothing.
- Colour knobs never sit in a transform group, so T (colour's temperature) and T (center) never collide.

**Assumptions:**
- Unconfirmed: the gate is unit tests for the classifier, key map and stroke maths, an Xvfb GUI script, and an AppImage UAT round.
- Unconfirmed: the rates above are starting values, tuned at UAT.

## Phase 173.1: Glyph and HUD mockup

- [ ] M173.P1.T1 — Build a Qt mockup of every kind's reveal glyph, widget cursor and HUD
  - files: `build/m173-mockup/` (standalone Qt program and screenshot script; not shipped)
  - approach: Reuse M119's disc/ring drawing (copy its ring function if linking is awkward). Render translate, rotate, scale, size, skew, generic 1D and generic 2D spatial as reveal discs with direction labels outside the ring, plus the split HUD under each (knob name; Q/W/E/R/A for a lone knob, Q/W/E/R/T + 1/2/A for the transform group; gearing hints), in the theme colours. Start from the sketches in `## Scope`. Run under Xvfb (`build/deeprepro/run-gui.sh` recipe) and save one PNG per kind plus the group HUD.
  - verify: PNGs exist for all seven kinds and the group HUD; send them to the user with SendUserFile.
  - size: M
- [ ] M173.P1.T2 — User approval of the mockup
  - files: this milestone file (`## Decisions`)
  - approach: Iterate on T1's screenshots until the user approves; record the approved designs, and any changes to them, as decisions.
  - verify: an approval decision dated and recorded.
  - size: S

## Phase 173.2: Knob roles and stroke maths (Engine)

- [ ] M173.P2.T1 — Add a value-role hint to numeric knobs and fill it from OFX
  - files: `Engine/KnobTypes.h`, `Engine/KnobTypes.cpp`, `Engine/OfxParamInstance.cpp`
  - approach: Add `enum ValueRoleEnum { eValueRoleNone, eValueRoleTranslate, eValueRoleRotate, eValueRoleScale, eValueRoleSize, eValueRoleSkew }` with `setValueRole`/`getValueRole` on the shared numeric knob base used by `KnobDouble` and `KnobInt` (pick the class both derive from; do not serialize it). In `OfxParamInstance.cpp`, where the double type is read, map Angle → rotate, Scale → scale, XY/XYAbsolute (and normalised variants) → translate.
  - verify: a gtest in `Tests/` that creates `KnobDouble`s and round-trips each role; the release build compiles.
  - size: M
- [ ] M173.P2.T2 — Set roles on native nodes and from the transform interact
  - files: `Engine/Nodes/Transform/Transform.cpp`, `Engine/Nodes/Filter/Blur.cpp`, `Engine/NodeOverlay.cpp`, `Engine/Node.h`
  - approach: In `Node::addTransformInteract`, set translate/rotate/scale/skew roles on the knobs passed in and record the interact's knob set so the GUI can query it (`Node::getTransformInteractKnobs()` returning translate, rotate, scale, skewX, skewY, center, or an empty struct). Set `size` on Blur's `size`. Grep other native nodes under `Engine/Nodes/` for translate/rotate/scale/size knobs and set their roles too.
  - verify: a gtest that instantiates native Transform and Blur and checks each knob's role and `getTransformInteractKnobs()`.
  - size: M
- [ ] M173.P2.T3 — Knob kind classifier with name/label fallback
  - files: new `Engine/GlobalModeKind.{h,cpp}`, `Engine/CMakeLists.txt` (or the sources list it uses), `Tests/GlobalModeKind_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: `classifyGlobalModeKind(const KnobIPtr&)` → colour / translate / rotate / scale / size / skew / genericSpatial2D / generic / ineligible. Order: colour knobs → colour; explicit role wins; else name/label heuristic (case-insensitive word match); else a 2D spatial knob → genericSpatial2D; else a numeric knob → generic; non-numeric knobs, or knobs with every dimension expression/link-driven → ineligible.
  - verify: `GlobalModeKind_Test` covers each branch, including a heuristic hit, a role overriding a misleading name, and an all-driven knob.
  - size: M
- [ ] M173.P2.T4 — Per-kind stroke maths and key map
  - files: new `Engine/GlobalModeStroke.{h,cpp}`, `Tests/GlobalModeStroke_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: Pure functions mirroring `ColorGlobalModeStroke`: given kind, start values per dimension (one or two knobs for skew), increment, dimension count, isInt, lock (all / dimension n), gearing and accumulated dx/dy, return new values. Rates as in `## Scope`; scale multiplicative; int rounding from accumulated travel. Also `classifyGlobalModeValueKey(key, inGroup)` mapping Q/W/E/R/A (lone) and Q/W/E/R/T + 1/2/A (group) to lock or kind-switch actions, ignoring locks beyond the dimension count.
  - verify: `GlobalModeStroke_Test` checks every kind's mapping, gearing, locks, int accumulation and both key maps.
  - size: M

## Phase 173.3: Generic global mode (Gui)

- [ ] M173.P3.T1 — Extract a target interface from the colour global mode
  - files: `Gui/ColorTrackballGlobalMode.{h,cpp}`, `Gui/ColorTrackballTarget.{h,cpp}`, new `Gui/GlobalModeTarget.h`
  - approach: Introduce an abstract `GlobalModeTarget` (knob label, panel, HUD attach/detach and highlight hooks, begin/move/end/abandon stroke, undo/redo between strokes, key handling, glyph widget factory). Make `KnobGuiColor`'s current behaviour one implementation, so the controller holds a `GlobalModeTarget` rather than `KnobGuiColor*`. Keep event filter, edge warp, wheel notches, right-click exit and gearing in the controller. Behaviour-preserving refactor only.
  - verify: the existing colour global-mode tests (`Tests/ColorTrackballHud_Test.cpp` and M119's GuiTests) pass unchanged on the batched build.
  - size: L
- [ ] M173.P3.T2 — Multi-knob value edit session
  - files: new `Gui/ValueKnobEditSession.{h,cpp}`, `Gui/CMakeLists.txt`, `Tests/ValueKnobEditSession_Test.cpp`
  - approach: Like `ColorKnobEditSession` but for N (knob, dimension) slots across up to two knobs: `begin` snapshots start values, `move` writes through the knobs with begin-time values as old values, `end` pushes one undo command spanning every touched knob, `abandon` restores. Skip expression- and link-driven dimensions with the same predicate as `ColorKnobEditSession.cpp:53-59`. Animated knobs key at the current frame via `KnobUndoCommand`.
  - verify: the gtest drives a two-knob (skewX/skewY) stroke and asserts one undo step restores both, a driven dimension is untouched, and an animated knob gets a key at the current time.
  - size: M
- [ ] M173.P3.T3 — Value-knob target for single knobs
  - files: new `Gui/ValueKnobGlobalModeTarget.{h,cpp}`, `Gui/KnobGuiValue.{h,cpp}`, `Gui/CMakeLists.txt`
  - approach: Implement `GlobalModeTarget` for a `KnobGuiValue` using P2.T3's kind, P2.T4's stroke maths and key map, and P3.T2's session. Attach the HUD into the knob's column the way `KnobGuiColor::attachGlobalModeHud` does, and highlight the row. No engage button.
  - verify: a GuiTest that engages a Blur `size` target programmatically, applies a stroke, and checks both dimensions moved uniformly and Q locks to x.
  - size: L
- [ ] M173.P3.T4 — Transform-group target
  - files: new `Gui/TransformGroupGlobalModeTarget.{h,cpp}`, `Gui/CMakeLists.txt`
  - approach: A `GlobalModeTarget` over a node's `getTransformInteractKnobs()`. Q/W/E/R/T switch the active kind (translate/rotate/scale/skew/center); 1/2/A lock axes; skew drives skewX/skewY through one session. Engaging any interact knob creates this target with that knob's kind active. The HUD names the active knob and shows the group legend.
  - verify: a GuiTest on a native Transform: engage `rotate`, press Q and stroke (translate changes), press R and stroke (both skews change in one undo step), press 2 (only y moves).
  - size: M
- [ ] M173.P3.T5 — Ctrl+E targeting and stepping across all eligible knobs
  - files: `Gui/Gui50.cpp`, `Gui/ColorTrackballTarget.{h,cpp}`, `Gui/ColorTrackballGlobalMode.cpp`, `Gui/GuiApplicationManager10.cpp`
  - approach: Ctrl+E: hovered numeric knob → its target (group target if it's an interact knob); else last-touched eligible knob of any kind on the top node (track last-touched for value knobs as colour does); else today's colour fallback; else the node's first classified knob. Replace `orderedColorKnobs` stepping with `orderedGlobalModeKnobs` over every eligible knob in panel order, keeping the end-of-list HUD messages ("First knob." / "Last knob."). Update the shortcut description `kShortcutDescActionColorTrackball`.
  - verify: a GuiTest builds a Transform + Grade bin and checks hover targeting, last-touched fallback and Ctrl+Down stepping from a colour knob to the next numeric knob.
  - size: M
- [ ] M173.P3.T6 — Per-kind reveal glyphs, widget cursor and HUD legend
  - files: new `Gui/ValueModeDiscWidget.{h,cpp}`, M119's disc/ring source (re-scout the name), `Gui/ColorTrackballHud.{h,cpp}`, `Gui/CMakeLists.txt`
  - approach: Draw the P1.T2-approved glyphs for each kind on the shared ring function, plug them into both feedback modes M119 kept (crosshair + reveal, widget cursor), and give the HUD per-kind direction labels and key legends.
  - verify: an Xvfb screenshot per kind matches the approved mockup, checked by eye and sent to the user.
  - size: M

## Phase 173.4: Verification and UAT

- [ ] M173.P4.T1 — Xvfb GUI script for value-knob global mode
  - files: `build/m173-gui/` (script and fixtures, under `build/` per the Xvfb recipe)
  - approach: Drive real key and mouse events on the AppImage. Ctrl+E over Blur `size`, stroke, check the value moved and one Ctrl+Z undoes it. Ctrl+E on Transform `rotate`, then Q/W/E/R/T switching and 1/2 locks. Ctrl+Down from a Grade colour knob onto a numeric knob. Right-click exit. Capture screenshots of every kind.
  - verify: the script reports zero FAIL; screenshots are sent to the user.
  - size: M
- [ ] M173.P4.T2 — UAT AppImage
  - files: `build/appimages/`
  - approach: Package the fast preset with `package.sh` and hand it to the user with the rates listed for tuning; record their feedback as decisions.
  - verify: the AppImage launches with `--version`; the user signs off.
  - size: S

**Verification gate:** release build plus both ctest legs green (including `GlobalModeKind_Test`, `GlobalModeStroke_Test`, `ValueKnobEditSession_Test`, the new GuiTests and the unchanged colour global-mode tests); `build/m173-gui/` script zero FAIL under Xvfb; the user approves per-kind screenshots and signs off the UAT AppImage.

## Decisions

- 2026-10-24 — **Scope set by the user (grilled):** extend global mode to every double/int knob; classify by explicit role hint first, name/label heuristic as fallback: the user wants the special widgets on knobs that declare intent, with a heuristic catching the rest.
- 2026-10-24 — **No engage button on non-colour rows; Ctrl+E only (user):** keeps plain rows uncluttered.
- 2026-10-24 — **Ctrl+E targets the hovered knob, else last-touched of any kind, else today's colour fallback, else first classified (user).** Ctrl+Up/Down steps through every eligible knob in panel order (user).
- 2026-10-24 — **Rotate is a linear drag, not a swept angle (user).** Scale and size are uniform by default (user). Rates are each knob's increment per px with colour's gearing; translate is 1 image px per screen px (user).
- 2026-10-24 — **Dimension keys are context-dependent (user):** on a lone knob Q/W/E/R lock dimension 1–4 and A = all; in a transform group Q/W/E/R/T switch translate/rotate/scale/skew/center and digits 1/2 lock x/y, A = both. The user chose this over moving the locks to digits everywhere.
- 2026-10-24 — **Transform group = any node that registered `addTransformInteract` (user):** its knob list names each role exactly, and future nodes get it automatically.
- 2026-10-24 — **Skew gets its own kind spanning skewX/skewY; center reuses translate's glyphs and mapping (user).**
- 2026-10-24 — **Glyphs are mocked up in Qt and approved before implementation (user)**, as M119 settled its HUD.
- 2026-10-24 — **Out (user):** bool/choice/string/parametric knobs, and viewer-side gestures.
- 2026-10-24 — **Expression/link-driven dimensions and animated knobs follow `ColorKnobEditSession`'s rules (code finding):** a gesture skips driven dimensions; keyframes go through `KnobUndoCommand` at the current frame.
