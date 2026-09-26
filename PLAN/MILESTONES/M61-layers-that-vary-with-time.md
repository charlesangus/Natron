# Milestone 61: Layers that vary with time

M34's second Codex round asked for a test proving that Shuffle validates at the render's time, not the timeline's. The PM declined it: "No node reports layers that vary with time." That's wrong. An input's layers can change from frame to frame, and this milestone makes sure Natron handles that and tests it. Three real graphs must each report different layers on different frames:

1. **EXR sequence with per-frame AOVs.** A Read of a sequence where frame 1 carries `diffuse` and frame 2 doesn't.
2. **Switch with an animated `which`.** Keyed between a Read that carries `diffuse` and one that doesn't.
3. **Animated disable upstream.** A node that produces a layer, with its Disable knob keyed on at frame 2.

Any of these that doesn't vary today is a bug, and it's fixed here (user decision, 2026-09-24). The scout found likely causes for all three:
- (1) ReadOIIO's `getClipComponents` reports `_outputLayerMenu`, which is built once from one frame (`ReadOIIO.cpp` ~769–800, `buildOutputLayerMenu` ~1284).
- (2) The default passthrough for a non-multiplanar effect is `node->getPreferredInput()`, which ignores time (`EffectInstance::getComponentsNeededDefault`, ~`EffectInstance.cpp:4366-4386`). The OFX Switch picks its input only through `isIdentity(time)`.
- (3) The Disable knob is created with `setAnimationEnabled(false)` and `setIsMetadataSlave(true)` (`Node.cpp` ~2214-2222), and `Node::isNodeDisabled()` reads `getValue()` with no time argument (`Node.cpp` ~6078).

Execution notes:
- The `natron-dev` container is single-tenant, so one build runs at a time. Launch builds detached with a done-marker.
- The debug build defines NDEBUG, so tests use EXPECT/ASSERT, not assert().
- GUI checks run under Xvfb (recipe `build/deeprepro/run-gui.sh`, fixtures under `build/`).
- openfx-io changes go through the `charlesangus/openfx-io` fork, followed by an `OPENFX_IO_REF` bump in `tools/ci/local/fetch-assets.sh`. M57.P1.T1 and M34.P4.T5 are the precedent.

## Phase 61.1: Fixture and reproduction

- [x] M61.P1.T1 — Add a two-frame EXR sequence whose layers differ per frame
  - files: `Tests/fixtures/make-flat-layers-seq-fixture.py` (new, modelled on `make-flat-layers-fixture.py`), `Tests/fixtures/flat-seq-layers.0001.exr`, `Tests/fixtures/flat-seq-layers.0002.exr`
  - approach: frame 1 is 8×8 half with RGBA, `diffuse` and `specular`, using the same per-layer constant values as `flat-three-layers.exr`, so existing pixel assertions carry over. Frame 2 is RGBA only, with the same RGBA values. Commit the generated files next to the script, as the other fixtures are.
  - verify: `oiiotool --info -v` in the container lists `diffuse.*`/`specular.*` channels for 0001 and only `R,G,B,A` for 0002.
  - size: M
- [x] M61.P1.T2 — Write a reproduction test for each of the three graphs
  - files: `Tests/TimeVaryingLayers_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: each test builds its graph and asserts that `getAvailableLayers(t, ViewIdx(0), -1, …)` on the node under test includes `diffuse` at t=1 and not at t=2. Call it at both times, in both orders, with the timeline parked on the *other* frame, so current-frame leakage fails the test.
    - (a) Read(`flat-seq-layers.####.exr`), frames 1–2.
    - (b) OFX Switch (`net.sf.openfx.switchPlugin`): input 0 is Read(`flat-three-layers.exr`), input 1 is Read(`flat-rgba-only.exr`), `which` is keyed 0@1 and 1@2.
    - (c) Read(`flat-rgba-only.exr`) → native Shuffle writing a constant into a new `diffuse` layer → NoOp. The Shuffle's Disable is keyed off@1 and on@2; query the NoOp. If Disable can't be keyed yet, that's the failure being reproduced.
    
    Prefix any test that fails today with `DISABLED_`. Its fix task below removes the prefix. Record pass/fail per graph, with the observed layer lists, in this file's `## Decisions`.
  - verify: `ctest -R TimeVaryingLayers` is green, with only the failing cases disabled; the Decisions entry names which graphs fail and why.
  - size: M

## Phase 61.2: Make every graph vary with time

Skip (strike through) any task whose P1.T2 test already passes.

- [x] M61.P2.T1 — Follow a time-dependent identity when reporting passthrough layers
  - files: `Engine/EffectInstance.cpp`, `Engine/EffectInstance.h`, `Tests/TimeVaryingLayers_Test.cpp`
  - approach: in `getComponentsNeededDefault`, and anywhere else the passthrough input is chosen for layer reporting, ask `isIdentity_public` at `(time, view)` first. If the effect is identity onto input k at time t', report input k's layers at t', and set `passThroughInputNb`/`passThroughTime` to match. Otherwise keep `getPreferredInput()`. The actions cache is already keyed by `(hash, time, view)`. Check that `isIdentity` doesn't recurse into layer queries: M34 made Shuffle's `isIdentity` validate first. Guard that recursion rather than dropping the validation.
  - verify: the Switch test (b) runs undisabled and passes. `ctest -R 'Shuffle|Layer|WriteAllLayers'` and the full debug ctest stay green.
  - size: L
- [x] M61.P2.T2 — Make Disable animatable and honor it at the render's time in the engine
  - files: `Engine/Node.h`, `Engine/Node.cpp`, `Engine/EffectInstance.cpp`, `Tests/TimeVaryingLayers_Test.cpp`
  - approach: P1.T2 found the knob already accepts keys (`setValueAtTime` warns but keys it) — the real gap is that layer reporting never consults Disable: `getPresentLayers`/`getComponentsNeededAndProduced_public` report the Shuffle's produced `diffuse` even where it's disabled, so a disabled node must report its passthrough input's layers at that time. Add `Node::isNodeDisabled(double time) const`, which reads the knob `getValueAtTime(time)`, with the group and IO-container recursion also at `time`. Switch every render, identity, layer and RoD path in `EffectInstance.cpp` (for example ~1932, ~1972, ~3860, ~3963) to the timed form, using the render/action time. Keep the untimed form for UI callers only, reading the current frame. Enable animation on the knob. `setIsMetadataSlave(true)` needs a decision: metadata is time-invariant, so either metadata is computed as if the node were enabled whenever the knob is animated, or it follows the current frame. Pick the option that keeps downstream metadata stable across frames and record it in `## Decisions`. P2.T3 handles serialization and the GUI.
  - verify: the disable test (c) runs undisabled and passes. A new render test keys Disable on a Grade, then renders frames 1 and 2 with the timeline parked on the other frame, and gets graded pixels at 1 and ungraded at 2. Full debug ctest green.
  - size: L
- [ ] M61.P2.T3 — Keyed Disable round-trips through the project and shows per frame in the GUI
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGui.h`, `Engine/Node.cpp`, `Tests/TimeVaryingLayers_Test.cpp`
  - approach: the node graph's disabled look (`NodeGui` ~ its `isNodeDisabled()` uses) follows the current frame and refreshes on timeline change, the way other per-frame node-box state does. The Disable checkbox shows keyframe colouring like any animated knob. Check that save → load keeps the keys. Knob curves already serialize, but confirm nothing special-cases `kDisableNodeKnobName`.
  - verify: gtest saves and reloads a project with keyed Disable, and the keys survive. Xvfb: scrubbing frames 1→2 turns the node box to disabled and back. Share screenshots with the user before sign-off.
  - size: M
- [x] M61.P2.T4 — ReadOIIO reports the layers of the frame being asked about
  - files: fork `charlesangus/openfx-io`: `OIIO/ReadOIIO.cpp`; this repo: `tools/ci/local/fetch-assets.sh` (the `OPENFX_IO_REF` bump), `Tests/TimeVaryingLayers_Test.cpp`
  - approach: in `getClipComponents`, resolve `getFilenameAtTime(args.time)`, read that file's subimage specs (reuse the ImageCache/spec path `buildOutputLayerMenu` uses), and report that frame's layers. Fall back to `_outputLayerMenu` only when the frame can't be read. Leave the output-layer menu as it is: it's the UI list, not the per-frame truth. Check whether the Natron-side Read container (`ReadNode`) caches layer lists anywhere, and key any such cache by time. Open the fork PR on a branch, pin the branch commit as M34.P4.T5 did, and leave the fork PR for the user.
  - verify: the sequence test (a) runs undisabled and passes after the rebuilt plugin bundle. `ctest -R 'Read|WriteAllLayers|Shuffle'` stays green.
  - size: L

- [ ] M61.P2.T5 — Thicken the node graph's disabled cross to about the width of the default pipe
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGui.h`
  - approach: at least 3x the current line width, set as its own literal copied from the default pipe's current width. It deliberately doesn't depend on the pipe width. The lines stay inside the node box, and the colour is unchanged.
  - verify: Xvfb release screenshots of a disabled node next to a pipe; the user signs off.
  - size: M

## Phase 61.3: Shuffle validates at the render's time

- [x] M61.P3.T1 — Add the test M34's review asked for, on all three graphs
  - files: `Tests/ShuffleRender_Test.cpp`, `Engine/Nodes/Channel/Shuffle.cpp` (only if the test exposes a bug)
  - approach: for each graph from P1.T2, feed a Shuffle whose explicit row reads `diffuse.r` into `Color.r`.
    - Park the timeline on frame 2 and render frame 1: it succeeds, with the fixture's diffuse value in R.
    - Park the timeline on frame 1 and render frame 2: it fails with the missing-layer persistent error naming `diffuse`.
    - Render frame 1 again: it succeeds and clears the error.
    
    Repeat once with a ShuffleCopy reading `diffuse` from input "1". Reuse the `renderExpectingFailure`/`render` helpers, with a frame argument added if they lack one. If any case fails, fix the validation path so it uses the render's `(time, view)`. `getChannelCount` (~`Shuffle.cpp:287`) reads the timeline frame, so check it's reachable only from UI code.
  - verify: `ctest -R ShuffleRender` green, including the new cases; full debug ctest green.
  - size: M

- [ ] M61.P3.T2 — Any read of a missing layer or channel fails the render, explicit row or default
  - files: `Engine/Nodes/Channel/Shuffle.cpp`, `Engine/Nodes/Channel/Shuffle.h`, `Tests/ShuffleRender_Test.cpp`, `Tests/Shuffle_Test.cpp`
  - approach: the user's rule: a missing channel is an error, always. Today `checkExtraChannelsPresent` (~`Shuffle.cpp:875`) checks explicit mapping rows only (M34's design). Separately, `getEffectiveSource` silently turns a default `inK.i` into 0 when `i` is beyond slot K's layer's channel count, and treats Color as RGBA whatever the input carries. Both go.
    - `getEffectiveSource`: a default source stays `inK.i`, with no zeroing beyond the channel count. Only a None slot still yields 0.
    - `checkExtraChannelsPresent`: check every output channel the render produces (`outChannels`), through its effective source. An explicit `eInput` row or a default `inK.i` fails when the input's `getPresentLayers(time, view, …)` lacks the slot's layer, or when that layer lacks the channel, resolved by name as the explicit-row path does today (`planeChannelName`). Use the existing message format and persistent-error/clear path.
    - This deliberately makes a fresh Shuffle (In 1 = Out 1 = Color) on an RGB-only input fail on A, and ShuffleCopy's default `A ← in1.A` fail when input "1" has no alpha. The user fixes it by setting that row to 0 or 1.
    - These stay silent: a None slot (an explicit choice of 0) and a disconnected input (renders 0, as M34 test (e)).
    - `isIdentity` (~`Shuffle.cpp:597`) already refuses the shortcut when the check fails, so the RGB-only case renders and errors instead of passing through. Confirm that.
    - Update the mapping knob's tooltip ("…or 0 when that slot is None or has no such channel") and the ShuffleCopy description if they describe the old zeroing.
  - verify:
    - `ImplicitDefaultOnAMissingLayerRendersZeroSilently` and `ImplicitAlphaOfAnRgbOnlyColorRendersZeroSilently` are renamed and inverted: each fails with a persistent error naming the channel (`diffuse.…` / `Color.A`).
    - `EffectiveSourceZeroesAnImplicitChannelTheSlotLacks` is inverted: `getEffectiveSource(1, 3)` with In 1 = `diffuse` returns `in1.3`, and the check fails on it.
    - New: In 1 = `diffuse` → Out 1 = Color, with an explicit A ← 0 row, renders (0,1,0,0) with no error. Any M34 pixel test that relied on the zeroing is updated to set that row, never deleted.
    - New time-varying case on graph (a): In 1 = `diffuse`, Out 1 = Color, no explicit rows. It errors on frame 2 and clears on frame 1.
    - ShuffleCopy with input "1" disconnected still gives A = 0 and no error.
    - `ctest -R 'Shuffle|TimeVaryingLayers'` green; full debug ctest green.
  - size: M

## Phase 61.4: Checkpoint

- [ ] M61.P4.T1 — Package an AppImage and write a UAT script for time-varying layers
  - files: `build/appimages/M61-<sha>.AppImage`, `build/appimages/M61-uat.md`
  - approach: release `package.sh`. The UAT script walks through the three graphs in the GUI. For each one: scrub frames 1↔2 and watch the Shuffle panel, the viewer, and the error badge. The error appears on frame 2 only and clears on frame 1. The keyed Disable shows in the node graph.
  - verify: the AppImage launches under Xvfb; the user runs the UAT script and signs off.
  - size: S

**Verification gate:** `ctest -R 'TimeVaryingLayers|ShuffleRender'` green with no `DISABLED_` cases left in `TimeVaryingLayers_Test.cpp`; full debug ctest green; the openfx-io fork PR is open, and `OPENFX_IO_REF` pins its commit; P2.T3's screenshots and P4.T1's UAT are signed off by the user.

## Decisions

- 2026-09-24 — **Layers vary with time, and every graph must report it per frame** (user): M34's decline rested on a false premise. Sequences with per-frame AOVs, an animated Switch and an animated Disable must each change a downstream node's layers across frames. Any that doesn't is a bug, fixed in this milestone. See `DECISIONS/2026-09-24-layers-vary-with-time.md`.
- 2026-09-24 — **View is out of scope**: the declined M34 test also mentioned view. Only time was raised, and no fixture varies layers per view. A view case can be added later if multi-view EXRs need it.
- 2026-09-25 — **Repro tests assert on `getPresentLayers`, not `getAvailableLayers`** (P1.T2): with inputNb -1, `getAvailableLayers` merges the sticky project layer registry, so a layer seen on any frame shows on every frame. `getPresentLayers` (produced + passthrough, no registry) is what Shuffle's render-time validation consumes (`Shuffle.cpp` ~560/758/912). Later tasks' verify steps mean the present-layer assertions.
- 2026-09-25 — **All three graphs fail today; all three tests are `DISABLED_`** (P1.T2, code `afdaf7525`). Observed with the timeline parked on the other frame (position made no difference):
  - (a) Read of `flat-seq-layers.####.exr`: t=1 and t=2 both report Colour, diffuse, specular. `ReadOIIOPlugin::getClipComponents` ignores `args.time` and lists `_outputLayerMenu`.
  - (b) Switch: t=1 and t=2 both report Colour, diffuse, specular. Non-multiplanar, so `getComponentsNeededDefault` passes through `getPreferredInput()` (Read A) regardless of `which`.
  - (c) Read → Shuffle → Dot: t=1 and t=2 both report Colour, diffuse. Disable keys fine; layer reporting never consults Disable, so the disabled Shuffle's produced layer leaks.
- 2026-09-25 — **Passthrough follows a time-dependent identity; the identity query is uncached** (P2.T1, code `1987bba2d`): `getComponentsNeededDefault` asks `isIdentity_public(false, …)` at the query's (time, view) and reports the identity input's layers. It uses `false` because the identity cache is keyed only on (hash, time, view): caching an empty Roto's identity during a layer query made `RotoLayerTest.RotoWritesAlphaOnlyIntoColor` read a stale answer after the shape was added. Re-entry is guarded by a per-effect TLS flag (`EffectTLSData::resolvingLayersPassThrough`), which also skips caching the fallback answer. Multiplanar plug-ins keep their own passthrough choice; only their upstream query moves to the declared passthrough time/view. Full debug ctest 482/483 (the one failure was the Read test, pending the plugin rebuild).
- 2026-09-25 — **ReadOIIO reports each frame's layers; pinned to the fork PR branch** (P2.T4, code `526df8690`): fork PR charlesangus/openfx-io#7 (branch `fix/read-layers-per-frame`, commit `23f8adc`) makes `getClipComponents` read the specs of `getFilenameAtTime(args.time)` and fall back to `_outputLayerMenu` only when the frame can't be read. `OPENFX_IO_REF` pins the branch commit; **re-pin to the merge commit on `master` once #7 merges**, since a squash + branch delete orphans it. `ReadNode` holds no layer cache. Each uncached call opens the file header (ImageCache is off for Natron ≥2.2); the ActionsCache bounds it to once per (hash, time, view). Verified after rebuilding the bundle: `ctest -R 'Read|WriteAllLayers|Shuffle|TimeVaryingLayers'` 104/104.
- 2026-09-25 — **Disable is animatable and read at the render's time** (P2.T2, code `1869a2f1c`): `Node::isNodeDisabled(double time)` drives identity, RoD, transform concatenation, OFX clip RoD, the viewer's input choice, and layer reporting (a node disabled at t reports its passthrough input's layers at t, via the same `getLayersPassThroughInput`). The untimed form is UI-only. Two further calls:
  - **Metadata:** when Disable varies with time (keys or expression), metadata is computed as if the node were enabled; only a static "on" gives passthrough metadata (`isNodeDisabledAtAllTimes()`). A time change never refreshes metadata, so "follow the current frame" would freeze whichever frame last refreshed. Side effect: a lifetime-limited node's metadata is always "as enabled".
  - **Writers:** `AppInstance`'s render-job skip uses `isNodeDisabledAtAllTimes()`; a writer disabled on some frames just writes nothing there. Side effect: a writer whose lifetime excludes the current frame is no longer skipped wholesale.
  - Caches stay correct per frame because every actions/image cache key carries time and view; the hash is age-based.
  - New `ChannelSetRenderTest.GradeDisableKeyedPerFrameIsHonouredAtTheRenderedFrame` (may also pass on old code via render-thread TLS time; the layer test is the one that failed before). Full debug ctest 485/485.
- 2026-09-25 — **Node box follows a keyed Disable per frame** (P2.T3 code, `ee62af5c0`): `Knob::onTimeChanged` never re-emits `disabledKnobToggled` for an animated knob, so `NodeGui` now connects the timeline's `frameChanged` (RotoPanel/TrackerPanel precedent) and refreshes only when the Disable knob is animated. No serialization special-case existed; `KeyedDisableSurvivesSaveLoad` passes. Checkbox stays open until the Xvfb scrub screenshots are signed off.
- 2026-09-25 — **Shuffle validates at the render's time; a successful render retires a stale missing-layer error** (P3.T1, code `0006da64c`): `layerChannelCount`/`getEffectiveSource`/`slotIsRead` take an explicit time (render → `args.time`; the sublabel, Python `ShuffleMapParam` and the matrix widget pass the current frame as UI callers). The new tests exposed that the missing-channel persistent error was only cleared by a mapping edit or `refreshChannelSelectors()`, so an error from rendering frame 2 survived a successful frame-1 render; `renderRoI` now calls the extracted `Node::clearStaleChannelSelectorMessage()` when `checkSelectedChannelsPresent` passes. Tests read `diffuse.g` (1.0), not `diffuse.r` (0, indistinguishable from empty). Full debug ctest 490/490.
- 2026-09-26 — **Gate progress**: PR #34 opened against `main` (M34 already merged, so not stacked). The Codex review round posted 6 findings (3 major: Shuffle view not threaded, the TLS guard swallowing identity exceptions, the stale-message clear lacking ownership/atomicity; 2 minor: NodeGui ignoring expression-driven Disable, reverse-order tests reusing a warm cache; 1 nit comment); a fixer is working on all six. The release AppImage `build/appimages/M61-776ec2ca1.AppImage` and `M61-uat.md` are ready and will be repackaged after the fixes. The Xvfb scrub screenshots (`build/m61-gui/disable-f{1,2,1-again}.png`) show the cross on frame 2 only; they have been sent to the user and await sign-off.
- 2026-09-26 — **Disabled cross thickened to pipe width** (user, on reviewing the P2.T3 screenshots): at least 3x the current thickness, about the same width as the default pipe, with the number copied rather than derived: the cross must not change if the pipe width does (user). Added as P2.T5 and folded into the review-fix batch, because that batch already touches `NodeGui.cpp`.
- 2026-09-26 — **Review round 1 closed** (code `d92ef11e3`; all 6 findings fixed and replied to on PR #34). Channel-selector errors now have explicit ownership: a `persistentMessageFromChannelSelector` flag under the message mutex replaces the text-prefix matching. A passing check at any (time, view) clears only an owned error, which keeps scrub-back clearing. Accepted tradeoff: a passing writer render of frame 1 can clear a frame-2 viewer error, which reappears on the next frame-2 render. `getLayersPassThroughInput` now propagates exceptions, with an RAII guard on the TLS flag. Shuffle threads `ViewIdx`; UI callers use view 0. NodeGui gates on `hasAnimation()`. The same commit carries P2.T5: the cross pen is `kDisabledCrossPenWidth = 2.` (copied from `EDGE_PEN_WIDTH 2`; the old width was Qt's default 1, so this is 2x, not the 3x first asked). Full debug ctest 491/491. No second round: the fixes are targeted and each has a test.
- 2026-09-26 — **Awaiting user checks (stacked-PR rule: asynchronous)**: CI on PR #34 is green. `build/appimages/M61-d92ef11e3.AppImage` (sha256 `3d4f4cd9543a9bb17952c6cde4d148d03f1272c9fc5b6ed243ccdfe056483561`) and `M61-uat.md` (which adds an expression-driven-Disable step) are delivered. The v2 screenshots (`build/m61-gui/v2/`) show the cross at pipe weight on frame 2 only; the user is asked whether width 2 is enough. The AppImage must be launched with the devshell `LD_LIBRARY_PATH` stripped (`build/appimages/run-launch-check.sh`). P2.T3, P2.T5 and P4.T1 stay open until the user signs off.
- 2026-09-26 — **UAT round 1, Graph 1 FAILED** (user): a Read of `flat-seq-layers.0001.exr` (detected as a 1–2 sequence) shows diffuse/specular in its info panel, but the viewer's layer menu and the Shuffle's In 2 dropdown don't offer them, so the missing-layer check can't be set up and no error appears. The ctest on `getPresentLayers` passes, so either the menus take a different path or the AppImage's plugin is stale. A diagnosis agent is reproducing it in the AppImage under Xvfb. M61 is back to `doing` until this is fixed and the UAT re-run.
- 2026-09-26 — **UAT Graph 1 root cause: layer menus never relisted on time change** (code `0ed1db2ee`). The AppImage's plugin was not stale (`.text`/`.rodata` identical to `build/assets/Plugins`), and the engine answered correctly. Knob menus (`listLayersForKnob` at the current frame) and the viewer menu (`getPresentLayers(currentFrame)`) were built once and refreshed only on graph/metadata/input change. Before M61 the Read always reported frame 1's layers, which hid this. Fix: `EffectInstance::refreshExtraStateAfterTimeChanged` calls the new `Node::relistLayerKnobs()`, and `ViewerTab::onTimeLineTimeChanged` refreshes its layer combo with `keepAbsentSelection` so a chosen layer survives frames that lack it. Single-file Reads were never affected. New tests: `LayerMenusListReadLayersAtCurrentFrame`, `TimeChangeRelistsLayerMenus` (the ctest missed this because it passed explicit times and never checked menu refresh). Full debug ctest 493/493. Repackaged as `M61-0ed1db2ee.AppImage`; the UAT script now covers menu relisting.
- 2026-09-26 — **UAT round 2: menus pass; a read of a missing layer or channel always errors** (user). On `M61-0ed1db2ee.AppImage` the layer menus relist correctly. The Shuffle errored on an invalid In 2 but not on an invalid In 1. The difference wasn't the slot: the UAT's In 2 step uses an explicit mapping row, while In 1 feeds Out 1 through the default, and M34 made `checkExtraChannelsPresent` check explicit rows only (M34.P6 test (f)). A first draft of P3.T2 kept a missing *channel* read through a default silent, so a fresh Shuffle or ShuffleCopy on an RGB-only input wouldn't error on A. The user rejected that as a misfeature: "Missing channel = error, always." This reverses M34's explicit-rows-only check and `getEffectiveSource`'s zeroing beyond a layer's channel count. A None slot and a disconnected input stay silent. After P3.T2 lands, P4.T1 repackages the AppImage and adds In 1/default and RGB-only-alpha steps to `M61-uat.md` for the next round.
