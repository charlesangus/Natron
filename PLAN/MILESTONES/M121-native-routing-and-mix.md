# M121 - Native Routing And Mix

Native, redesigned replacements for the openfx-misc routing and mix nodes: `KeyMix`, `Difference` and `AdjustBBox` (AdjustRoD), plus moving the users of `Switch` over and retiring all four OFX IDs. The native `Switch` node itself is built by M137 - Native Base Class Flexibility, as its Route-kind proof. Each node here has a CPU path, a GPU path and tests. Switch is the most-used OFX node left: 11 instances across 7 bundled PyPlugs. Moving it native lets those PyPlugs run without OFX hops, and lets GPU regions run through them.

Shared conventions:
- Plain IDs at major 1, Nuke-like knob sets and scene-linear maths. The nodes don't copy OFX behaviour (`DECISIONS/2026-10-12-native-nodes-drop-ofx-legacy.md`), and there is no project compatibility (`DECISIONS/2026-09-18-clean-break-no-project-compat.md`).
- Nodes are declared on M137 - Native Base Class Flexibility's framework: a `NodeSpec`, an `ImagePipelineSpec` with its stages (Premult, MaskMix) and an `Op`. Mask, mix and "(Un)premult by" are stages, not base-class code. `NativeImageEffect` no longer exists.
- Registration, kernels, icons, docs and retirement go through the per-node fragment files from M120 - Native Node Port Kit (`<Node>.reg.inc`, `docs/<Node>.md`, `<kernel>.cmake`; one `Engine/OfxRetired/<ofx-plugin-id>` marker per retired ID). Don't edit the shared manifests.

Deps:
- M137 - Native Base Class Flexibility. It provides the framework and the native Switch.
- M120 - Native Node Port Kit.
- M101 - Clean-Sheet Merge Node. It builds the `IBinaryPixelOp` GPU kernel shape (M101.P4.T1) that KeyMix and Difference use. Multi-source point ops come from M137's Composite kind.

Scouted facts (scouted 2026-10-13 on main `338d9324a`; M85 - Native GPU Kernels facts from `origin/milestone/m85-native-gpu-kernels`; re-check at promotion):
- **OFX Switch** (`openfx-misc/Switch/Switch.cpp`):
  - `which` is an int, animated, range 0..127 (`:496-509`), with a display range that follows the connected inputs (`:136-145`).
  - `automatic` (`:511-520`, `:148-165`).
  - `isIdentity` always routes and `render` never runs (`:247-262`, `:206`). It concatenates transforms on Nuke-extension hosts.
- **OFX KeyMix** (`KeyMix/KeyMix.cpp`):
  - Inputs B, A, Mask (`:95-98`). Per-channel A select (`:81-92`).
  - Out = A where the A channel is on, else 0, blended against B by mask×mix. The RoD is the union of A and B, and the format is B's.
  - Identity cases at `:498-566`.
- **OFX Difference** (`Difference/Difference.cpp`): A = clean plate, B = subject. RGB = B, alpha = `clamp(gain·Σ(B−A)² − offset, 0, max)` (`:128-140`). `offset` 0, `gain` 1 (`:395-422`).
- **OFX AdjustRoD** (`AdjustRoD/AdjustRoD.cpp`): `addPixels` (w,h) pads every side (`:212-230`). `boundary` is Black or Nearest, default Nearest (`:395-418`). Identity at 0,0.
- **Native multi-input before M137 (superseded by its Composite and Route kinds; re-scout at promotion):**
  - `NativeImageEffect::render` fills only `src[0]` (`NativeImageEffect.cpp:462-469, 543-544`), with `kPixelKernelMaxInputs 4` (`PixelKernel.h:37,89`). M101.P2.T1 lifts this.
  - Dissolve (`Engine/Nodes/Merge/Dissolve.{h,cpp}`) is the 64-input template: inputs at `Dissolve.cpp:228-246`, `onInputChanged` range rescale at `:277-280`, `isIdentity` override at `:371-418`.
  - The base `isIdentity` (`NativeImageEffect.cpp:344-380`) always answers input 0, so Switch overrides it.
- **Users:**
  - **PyPlugs** create `net.sf.openfx.switchPlugin` and drive `which` by expression:
    - `EdgeBlur.py:347` (expr `:431`)
    - `Glow.py:553,619,665,1356` (exprs `:1514,1520,1523,1589`)
    - `ZMask.py:405` (`:462`)
    - `PIKColor.py:585` (`:1119`, inputs wired `:1082-1083`)
    - `LightWrap.py:1199,1221,1255` (`:1649-1655`)
    - `ZRemap.py:289` (`:337`)
    - None uses `automatic`.
  - **Engine:**
    - `Engine/Node.cpp:2816-2821` keeps KeyMix's R/G/B/A as A-select (a quad-adoption exception), with comments at `Node.cpp:4368,4437` and `NodePrivate.h:514`.
    - `OfxEffectInstance.cpp:1282,1337-1343` lists these IDs in the OFX `processAllLayers` set. That list becomes dead for retired IDs, which is harmless.
  - **Tests:** `NodeCategory_Test.cpp:189-192` (KeyMix resolves to Merge), and the KeyMix quad cases in `ChannelSetRender_Test.cpp:1286-1369` that M120.P3.T1 defers to this milestone.

## Scope

**In:** native `KeyMix`, `Difference` and `AdjustBBox` with CPU and GPU paths and tests; identity nodes such as Switch keeping GPU regions resident; moving the PyPlugs to native Switch and the other engine reference updates; retiring the four OFX IDs, `switchPlugin` included.

**Out:**
- `CopyRectangle` and `NoOp` stay on OFX (user, 2026-10-13); none.
- Merge's `difference` operator (per-channel |A−B|) belongs to M101 - Clean-Sheet Merge Node.
- Renaming the PyPlugs' other OFX nodes: M111 - Native Node Identity Cleanup.
- The native `Switch` node, its routing and its tests: M137 - Native Base Class Flexibility.

**Edge cases:**
- KeyMix with A unconnected is identity on B. With the mask unconnected, A covers everywhere (mask = 1), the same as Nuke.
- Difference with A missing passes B through with alpha 0. With B missing it outputs black.
- AdjustBBox with negative padding crops. Nearest boundary repeats the source edge; Black fills 0.

**Assumptions:**
- Unconfirmed: the plain ID is `AdjustBBox` (Nuke's name), not `AdjustRoD`.
- Unconfirmed: Difference stays in the Keyer category, and KeyMix and Switch are in Merge.

## Phase 120.1: Switch

- [ ] ~~M121.P1.T1 — Native Switch as pure identity routing~~ (cancelled 2026-10-19: moved to M137 - Native Base Class Flexibility as its Route proof; its brief and the Switch edge cases travel with it)
  - files: `Engine/Nodes/Merge/Switch.h`, `Engine/Nodes/Merge/Switch.cpp`, `Engine/Nodes/Merge/Switch.reg.inc`, `Tests/Native/NativeSwitch_Test.cpp`
  - approach:
    - Derive from `NativeEffectBase`, or `NativeImageEffect` with a FINAL `isIdentity`, modelled on Dissolve.
    - 64 optional image inputs named "0".."63". `which` is an animatable int whose display range follows the highest connected input (`onInputChanged`, as at `Dissolve.cpp:277-280`).
    - `isIdentity` returns the clamped `which` input at the same time and view. `getRegionOfDefinition` returns the chosen input's RoD. `render` is unreachable.
    - No `automatic` knob (user).
    - Concatenation: forward `getTransform` from the chosen input if the base supports it, otherwise leave it out.
  - verify: at the phase build, `NativeSwitch_Test` covers routing for `which` 0/1/beyond-range, an animated `which` changing the output per frame, the no-input case, and no render call (a counting test effect upstream sees exactly one render per frame).
  - size: M

- [ ] M121.P1.T2 — Identity nodes don't break resident GPU regions
  - files: `Engine/GpuPlacement.cpp`, `Engine/GpuRegionTask.cpp`, `Tests/GpuPlacement_Test.cpp`
  - approach:
    - Check how placement treats a node whose route/identity hook (M137 - Native Base Class Flexibility) routes to an input. If it splits a region, make identity-at-input a pass-through edge: its consumer reads the routed input's resident view.
    - Cover Switch, and any native node at an identity setting, such as a default Grade.
  - verify: at the phase build, a new `GpuPlacement_Test` case shows Grade → Switch → Blur placed as one region with no download/upload between Grade and Blur.
  - size: L

## Phase 120.2: KeyMix and Difference

- [ ] M121.P2.T1 — Native KeyMix
  - files: `Engine/Nodes/Merge/KeyMix.h`, `Engine/Nodes/Merge/KeyMix.cpp`, `Engine/Nodes/Merge/KeyMix.reg.inc`, `Tests/Native/NativeKeyMix_Test.cpp`
  - approach:
    - A Composite op on M137's framework with `Stages{MaskMix}`: inputs B=0, A=1, Mask=2, with A and B as sources of the `StagePlan`.
    - Per-channel A select uses the standard layer/channel knobs.
    - Out = lerp(B, A, mask·mix) on selected channels, and B elsewhere. The host mask/mix supplies the blend. Don't add a second mask.
    - RoD is the union of A and B, and the output format is B's.
    - Identity on B when no channel is selected, A is unconnected, or the window misses A's RoD, through `Op::isIdentity`. The framework's composed identity already covers mix 0 and a non-inverted mask missing the window.
  - verify: at the phase build, the test covers a soft mask blend, channel subsets, the identity cases, and HDR values (>1 and negative) passing through.
  - size: M

- [ ] M121.P2.T2 — Native Difference
  - files: `Engine/Nodes/Keyer/Difference.h`, `Engine/Nodes/Keyer/Difference.cpp`, `Engine/Nodes/Keyer/Difference.reg.inc`, `Tests/Native/NativeDifference_Test.cpp`
  - approach:
    - A Composite op on M137's framework. Inputs A = clean plate (1) and B = subject (0, the main input).
    - RGB = B. Alpha = `max(0, gain·Σ_rgb(B−A)² − offset)` with no upper clamp, so scene-linear differences above 1 stay meaningful.
    - Knobs `offset` 0 and `gain` 1, plus host mask/mix on the alpha.
    - Missing-input cases as in the edge cases.
  - verify: at the phase build, the test covers known pixel pairs, a zero difference giving alpha 0, values above 1 not clamped, and the missing-input cases.
  - size: M

- [ ] M121.P2.T3 — Port the deferred KeyMix tests and references
  - files: `Tests/ChannelSetRender_Test.cpp`, `Tests/NodeCategory_Test.cpp`, `Engine/Node.cpp`, `Engine/NodePrivate.h`
  - approach:
    - Point `ChannelSetRender_Test.cpp:1286-1369` and `NodeCategory_Test.cpp:189-192` at the native `KeyMix` ID.
    - Keep the quad-adoption exception (`Node.cpp:2816-2821`) keyed on the native ID if native nodes go through that path. If they don't, delete the exception and its comments (`:4368,4437`, `NodePrivate.h:514`).
  - verify: at the phase build, both tests pass against native KeyMix.
  - size: M

## Phase 120.3: AdjustBBox and GPU paths

- [ ] M121.P3.T1 — Native AdjustBBox
  - files: `Engine/Nodes/Transform/AdjustBBox.h`, `Engine/Nodes/Transform/AdjustBBox.cpp`, `Engine/Nodes/Transform/AdjustBBox.reg.inc`, `Tests/Native/NativeAdjustBBox_Test.cpp`
  - approach:
    - A `numpixels` (w,h) pad in canonical units, with negative values cropping, and `boundary` Black/Nearest (default Black, like Nuke).
    - RoD = source RoD ± pad, and RoI = the requested window clamped to the source RoD.
    - A Point op on M137's framework whose source input declares `boundary = Zero | Nearest`, following the `boundary` knob. Identity at 0,0.
  - verify: at the phase build, the test covers pad, crop, both boundaries, and identity at 0.
  - size: M

- [ ] M121.P3.T2 — GPU kernels for KeyMix and Difference
  - files: `Engine/Gpu/Kernels/routing.slang`, `Engine/Gpu/Kernels/routing.cmake`, `Engine/Nodes/Merge/KeyMix.cpp`, `Engine/Nodes/Keyer/Difference.cpp`
  - approach:
    - Implement both as binary pixel ops on the shape M101.P4.T1 builds (`IBinaryPixelOp`), wrapped by `pixelwrap.slang`.
    - `canRenderOnGpu` accepts masks and mix: the wrap handles them.
  - verify: at the phase build, `NativeKeyMixGpu_Test` and `NativeDifferenceGpu_Test` (new, on the `NativeGpuParity` harness) match the CPU within 1e-6 absolute on RADV.
  - size: M

- [ ] M121.P3.T3 — GPU path for AdjustBBox
  - files: `Engine/Nodes/Transform/AdjustBBox.cpp`, `Engine/Gpu/Kernels/routing.slang`, `Tests/Native/NativeAdjustBBoxGpu_Test.cpp`
  - approach: a copy kernel with Black/Nearest boundary reads (reuse the boundary helpers in `pixelwrap.slang` if present), so AdjustBBox stays inside a region.
  - verify: at the phase build, the GPU parity test passes bit-exact on RADV.
  - size: M

## Phase 120.4: Retire and update users

- [ ] M121.P4.T1 — Move the PyPlugs to native Switch
  - files: `Gui/Resources/PyPlugs/EdgeBlur.py`, `Gui/Resources/PyPlugs/Glow.py`, `Gui/Resources/PyPlugs/ZMask.py`, `Gui/Resources/PyPlugs/PIKColor.py`, `Gui/Resources/PyPlugs/LightWrap.py`
  - approach:
    - Replace each `createNode("net.sf.openfx.switchPlugin", 1, …)` with the native `Switch` ID at major 1.
    - Drop the `channels` layer set if the native node has no such knob.
    - Keep the `which` expressions verbatim.
  - verify: at the phase build, `PyPlugInstantiate_Test` passes for these PyPlugs, and `PyPlugNativeParams_Test` passes or is updated for the new knob names.
  - size: M

- [ ] M121.P4.T2 — ZRemap and the retired markers
  - files: `Gui/Resources/PyPlugs/ZRemap.py`, `Engine/OfxRetired/<ofx-plugin-id>` (four markers), `Engine/Nodes/Merge/docs/{Switch,KeyMix}.md`, `Engine/Nodes/Keyer/docs/Difference.md`, `Engine/Nodes/Transform/docs/AdjustBBox.md`
  - approach:
    - Same Switch swap for `ZRemap.py:289`.
    - List `net.sf.openfx.switchPlugin`, `KeyMix`, `DifferencePlugin` and `AdjustRoDPlugin` as `Engine/OfxRetired/<ofx-plugin-id>` markers, one per ID.
    - Document the four nodes and their GPU tolerances, one per-node doc file each (M120 - Native Node Port Kit's scheme).
  - verify: at the phase build, `OfxRetired_Test` and `NativePluginList_Test` pass, `getPluginIDs()` has no retired ID, and a grep for `switchPlugin` in `Gui/` and `Engine/` finds nothing.
  - size: S

## Decisions

- 2026-10-13 — Switch drops `automatic` (user): only `which` is kept, since no bundled PyPlug uses automatic.
- 2026-10-13 — CopyRectangle and NoOp stay on OFX (user): low use, and CopyRectangle is a useful OFX test fixture for M120 - Native Node Port Kit.
- 2026-10-19 — Native Switch moves to M137 - Native Base Class Flexibility (user, M137 grill): no native Route node existed to prove the framework's route hook. This milestone keeps the PyPlug moves, the `switchPlugin` retirement and the identity-node placement task.
- 2026-10-13 — AdjustRoD becomes `AdjustBBox` (planner, Nuke's name): it follows the Nuke-like naming rule. Unconfirmed until promotion.

**Verification gate:**
- Full build and ctest pass, including the three nodes' CPU tests and the GPU parity tests on RADV.
- The new `GpuPlacement_Test` case shows a Switch inside one resident region.
- The 7 PyPlugs instantiate and render: an Xvfb check opens Glow and LightWrap and toggles their Switch-driven knobs.
- Xvfb screenshots of the three nodes' panels are shared before sign-off.
- The AppImage is built with `package.sh` for UAT.
- The four retired OFX IDs no longer load.
