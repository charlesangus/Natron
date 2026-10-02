# Milestone 66: Remaining OFX plugins accept alpha-only streams

The 12 "moderate" plugins from M65's survey process alpha-only (1-component) streams natively instead of through the host's widen-and-narrow fallback (M65 P8.T6):
- the 9 openfx-arena ImageMagick effects: Arc, Charcoal, Edges, Implode, Oilpaint, Polar, Reflection, Sketch and Tile;
- openfx-io's ReadEXR;
- openfx-misc's TimeBufferRead and TimeBufferWrite.

The older duplicate plugin registrations are re-registered under distinct IDs: HueCorrect 1.0 becomes `net.sf.openfx.HueCorrect1`, and the ImageMagick Text 5.7 becomes `net.fxarena.openfx.MagickText`.

Scout notes (2026-10-02):
- **The survey was wrong about a shared lock.** None of the nine effects use `MagickPluginHelperBase`. Each declares its own RGBA clips, checks RGBA in `render()`, and reads/writes `"RGBA"` in Magick. A shared helper goes in `Magick/MagickCommon.h`.
- **Magick mapping** (Q32 HDRI, `FloatPixel`):
  - Read with map `"I"`.
  - *Matte mode* (the geometric effects Arc, Implode, Polar, Reflection, Tile): `alphaChannel(CopyAlphaChannel)` after reading, export `"A"`. The result should equal the fallback's alpha.
  - *Gray mode* (the content effects Charcoal, Sketch, Oilpaint, Edges): no alpha, export `"I"`. These stylise the matte as a grayscale picture; say so in the plugin hint.
  - On the alpha path, skip the final Over + CopyAlpha onto opaque black.
  - The Matte param is a no-op on Alpha.
- **ReadEXR:** `kSupportsAlpha false` (`:90`); `decode()` requires RGBA/4 (`:563`), assumes `(int)Channel` is the offset (`:575`), and hard-codes a stride of 4 floats (`:604/:607`). The plugin is deprecated (evaluation 10), so tests create it by ID `fr.inria.openfx.ReadEXR`.
- **TimeBuffer:** the buffer already stores its components. Only the clip declarations (Read `:787,:795`; Write `:1306,:1312,:1319`) and the RGBA asserts (`:269,:272,:489,:940-944,:1162`) lock it to RGBA.
- **Telling native from fallback:** check `isSupportedComponent(0/-1, Alpha)` and `getMetadataNComps(0) == 1 && getMetadataNComps(-1) == 1`. The fallback reports 4. The fixture is `flat-alpha-only.exr`.

Execution notes:
- Stacked on M65: branch `milestone/m66-plugin-alpha-only-moderate` off `26cc11c9c`, PR against `milestone/m65-rgba-rgb-alpha-layers` (`DECISIONS/2026-09-22-stacked-milestone-prs.md`).
- Fork branches, all named `m66/alpha-only`:
  - misc off `721e35d7` (worktree `build/wt/m66-misc` from `build/openfx-misc-fork`);
  - io off `55ded52e` (`build/wt/m66-io` from `build/openfx-io-fork`);
  - arena off `387c98e4`, in a fresh full clone `build/openfx-arena-fork`. The old `build/assets/plugin-src/openfx-arena` is shallow, and `build/wt/m65-arena` is orphaned.
- Batches:
  - B0: P1.T1 (the PM, mechanical).
  - B1: P2.T1, P3.T1, P4.T1, P5.T1, P5.T2. P3.T1 and P4.T1 both append to `ColorViewsRender_Test.cpp`, so give them to one agent.
  - B2: P2.T2, P2.T3, P2.T4, P5.T3, then P2.T5.
  - Checkpoint: P6.T1 and P6.T2, then P6.T3.
  - One detached plugin build + Natron build + ctest per batch.
- The natron-dev container is single-tenant; never pgrep-wait. The debug build defines NDEBUG, so tests use EXPECT/ASSERT.

## Phase 66.1: Fork setup

- [x] M66.P1.T1 — Create m66 branches on the three forks at the pinned SHAs and point the local plugin build at them
  - files: `build/wt/m66-misc`, `build/wt/m66-io`, `build/openfx-arena-fork` (new), `build/build-m66-plugins.sh` (scratch, a copy of `build-m65-plugins.sh`)
  - approach:
    - `git -C build/openfx-misc-fork worktree add build/wt/m66-misc -b m66/alpha-only 721e35d7`, and the same for io at `55ded52e`.
    - Make a full clone of `charlesangus/openfx-arena`, then `checkout -b m66/alpha-only 387c98e4` and `submodule update --init`.
    - Remove the orphaned `build/wt/m65-arena`.
    - Re-clone `build/assets/plugin-src/openfx-io` at `55ded52e`; it is dirty today.
  - verify: each tree is on `m66/alpha-only` at the pin and clean; the scratch script builds all three.
  - size: S

## Phase 66.2: openfx-arena Magick effects take Alpha

- [x] M66.P2.T1 — Add a shared one-channel Magick read/write helper and use it in Charcoal
  - files: `Magick/MagickCommon.h`, `Magick/Charcoal.cpp`
  - approach:
    - The helper reads src as `"RGBA"`, or as `"I"` for Alpha. In matte mode it then calls `alphaChannel(CopyAlphaChannel)`.
    - It writes `"RGBA"` through the existing Over + CopyAlpha path, or for Alpha exports `"A"` (matte mode) or `"I"` (gray mode) directly.
    - Charcoal: add `ePixelComponentAlpha` to src and dst, require src comps == dst comps ∈ {RGBA, Alpha}, fix the ctor asserts, and use gray mode. Its hint says that on alpha it stylises the matte as a grayscale picture.
  - verify: arena builds; Charcoal is covered by P2.T5.
  - size: M

- [x] M66.P2.T2 — Accept Alpha in Sketch, Oilpaint and Edges via the helper (gray mode)
  - files: `Magick/Sketch.cpp`, `Magick/Oilpaint.cpp`, `Magick/Edges.cpp`
  - approach: as Charcoal. Edges on Alpha skips the IM≥7.0.8 alpha save/restore workaround and the Gray quantize. Confirm that the brightness `evaluate` on Red/Green/Blue hits the gray channel.
  - verify: arena builds; P2.T5 cases.
  - size: M

- [x] M66.P2.T3 — Accept Alpha in Arc, Implode and Polar via the helper (matte mode)
  - files: `Magick/Arc.cpp`, `Magick/Implode.cpp`, `Magick/Polar.cpp`
  - approach: clip declarations, the component check and the asserts; matte-mode read and write; the Matte param is a no-op on Alpha.
  - verify: arena builds; P2.T5 cases, where the alpha equals the fallback's alpha.
  - size: M

- [x] M66.P2.T4 — Accept Alpha in Reflection and Tile via the helper (matte mode)
  - files: `Magick/Reflection.cpp`, `Magick/Tile.cpp`
  - approach: export `"A"` from `container`. The containers stay rgba(0,0,0,0), so the composites carry alpha. Tile's per-tile fetch (`:271`) also goes through the helper. Matte is a no-op on Alpha.
  - verify: arena builds; P2.T5 cases.
  - size: M

- [x] M66.P2.T5 — Test that the nine Magick effects run natively on an alpha-only stream
  - files: `Tests/ColorViewsRender_Test.cpp`
  - approach: run a table of the nine IDs on `flat-alpha-only.exr` and assert:
    - `isSupportedComponent(0 and -1, Alpha)`;
    - `getMetadataNComps(0) == 1` and `getMetadataNComps(-1) == 1`;
    - a writeAll render has exactly one channel, "A", with finite values;
    - for Arc/Implode/Polar/Reflection/Tile, A equals the same effect run on an RGBA reader of the same matte.
  - verify: `build/m61ctest.sh ColorViewsRender` passes against the rebuilt Arena.ofx and fails against the `387c98e4` bundle.
  - size: M

## Phase 66.3: ReadEXR decodes Alpha

- [x] M66.P3.T1 — Let ReadEXR decode into an Alpha (1-component) buffer, with a test
  - files: openfx-io `EXR/ReadEXR.cpp`; `Tests/ColorViewsRender_Test.cpp`
  - approach: set `kSupportsAlpha` true, and let `decode()` take RGBA or Alpha. Stride = `pixelComponentCount*sizeof(float)`. Each channel's offset is mapped through the layout (Alpha: A→0), and channels the layout lacks are skipped. The test creates `fr.inria.openfx.ReadEXR` by ID.
  - verify: on `flat-alpha-only.exr`, `getMetadataNComps(-1) == 1` and the render is one channel "A" = 1; an RGBA fixture still reads 4 channels, unchanged.
  - size: M

## Phase 66.4: TimeBuffer carries Alpha

- [x] M66.P4.T1 — Let TimeBufferRead/Write take Alpha streams through the shared buffer, with a test
  - files: openfx-misc `TimeBuffer/TimeBuffer.cpp`; `Tests/ColorViewsRender_Test.cpp`
  - approach:
    - Add Alpha to Read's src/dst and Write's src/sync/dst, and drop the RGBA asserts.
    - Read's output follows its optional Source, and is RGBA when Source is unconnected.
    - If the buffer's components ≠ dst at render, post a persistent error instead of copying.
    - The test graph: alpha reader → Read.Source; Read → Write.Sync; reader → Write.Source; same bufferName; render Write@1, then Read@2.
  - verify: both nodes report metadata nComps 1 on the alpha stream; Read@2 renders one "A" channel equal to the frame-1 alpha; an RGBA graph still works.
  - size: M

## Phase 66.5: One plugin per ID

- [x] M66.P5.T1 — Re-register HueCorrect 1.0 as `net.sf.openfx.HueCorrect1`
  - files: openfx-misc `HueCorrect/HueCorrect1.cpp` (`:55` kPluginName "HueCorrect1OFX", `:74` kPluginIdentifier "net.sf.openfx.HueCorrect1"), `HueCorrect/net.sf.openfx.HueCorrect1.png/.svg` (copies), `CMakeLists.txt:271-274`
  - approach: exactly those edits; keep version 1.0 and `setIsDeprecated(true)`.
  - verify: Misc builds; `strings Misc.ofx | grep -Fx net.sf.openfx.HueCorrect1`.
  - size: M (resized from S: three files, including the CMake icon list)

- [x] M66.P5.T2 — Re-register the ImageMagick Text 5.7 as `net.fxarena.openfx.MagickText`
  - files: openfx-arena `Magick/Text.cpp` (`:34` kPluginName "MagickTextOFX", `:36` kPluginIdentifier "net.fxarena.openfx.MagickText"), `Magick/net.fxarena.openfx.MagickText.png/.svg` (copies)
  - approach: exactly those edits; keep 5.7 and deprecated.
  - verify: Arena builds; `strings Arena.ofx | grep -Fx net.fxarena.openfx.MagickText`.
  - size: M (resized from S: several files)

- [x] M66.P5.T3 — Test plugin identity and update the host references to the Magick Text ID
  - files: `Tests/PluginIdentity_Test.cpp` (new), `Tests/CMakeLists.txt`, `Engine/NodeDocumentation.cpp:239`, `tools/ci/local/fetch-assets.sh:567`
  - approach: assert one entry each for `net.sf.openfx.HueCorrect` (major 2), `net.sf.openfx.HueCorrect1` (1), `net.fxarena.openfx.Text` (6) and `net.fxarena.openfx.MagickText` (5). Add MagickText to the font-knob doc special case, and make the fetch-assets probe check MagickText as the Magick-backed ID.
  - verify: `build/m61ctest.sh PluginIdentity` passes against the rebuilt bundles.
  - size: M

## Phase 66.6: Checkpoint

- [ ] M66.P6.T1 — Publish the duplicate-ID and alpha-only Magick semantics decision
  - files: `PLAN/DECISIONS/2026-10-02-duplicate-ofx-plugin-ids.md`, then `docs/decisions/` at the gate
  - approach: older duplicates are re-registered under a distinct ID, never deleted. Magick content effects treat a matte as a grayscale picture; geometric effects treat it as alpha.
  - verify: the file is present and indexed.
  - size: S

- [ ] M66.P6.T2 — Push fork branches, open fork PRs, re-pin fetch-assets, rebuild assets, run full ctest
  - files: `tools/ci/local/fetch-assets.sh` (OPENFX_IO_REF, OPENFX_MISC_REF, OPENFX_ARENA_REF and their comments)
  - approach: push `m66/alpha-only` on the three forks, open PRs on `charlesangus/*`, and pin the branch SHAs. Run a fresh fetch-assets (re-cloned plugin sources, no dirty trees), then a detached build and the full `tools/ci/local/test.sh ctest debug`.
  - verify: the plugin pins match the three SHAs; full ctest green.
  - size: M

- [ ] M66.P6.T3 — Package the release AppImage for the user checkpoint
  - files: `build/appimages/M66-<sha>.AppImage`, `build/appimages/M66-uat.md`
  - approach: release `package.sh`. The UAT doc gives an alpha-only check per plugin, plus finding HueCorrect1 and MagickText in node search. The UAT is deferred to the parcel UAT.
  - verify: `build/appimages/run-launch-check.sh` passes.
  - size: M

**Verification gate:** all of the following hold:
- For each of the 12 plugins, an alpha-only stream reports Alpha supported on input and output, metadata nComps 1 on both, and renders to exactly one "A" channel. The matte-mode Magick effects match the fallback's alpha.
- One plugin per `net.sf.openfx.HueCorrect` and per `net.fxarena.openfx.Text`.
- Full debug ctest is green on re-pinned, freshly fetched assets.
- The M66 AppImage passes the launch check.

## Decisions

- 2026-10-02 — Duplicate plugin IDs (user): keep both implementations, and re-register the older one under a distinct ID rather than deleting it.
- 2026-10-02 — Elaborated from a consultant scout. Each pair already differs in major version, which OFX allows, so the rename is a policy choice rather than a collision fix. TimeBufferRead's output follows its Source input, and a buffer/output component mismatch is a persistent error.
- 2026-10-02 — **B1+B2 landed** (Natron `2d13b87da` P5.T3, `5abe974b6` tests for P2.T5/P3.T1/P4.T1; misc `2ea8f2fa` P5.T1, `0916b1ce` P4.T1; io `22ba2e7` P3.T1; arena `116a047` P2.T1+T2, `2c25aac` P2.T3+T4, `f008866` P5.T2). Full debug ctest was 658/662 before the fix round; then `ColorViewsRender|PluginIdentity` passed 32/32.
  - **Fixes in that round:** the matte-mode Magick effects now composite the alpha path the same way as the RGBA path. Arc and Polar resampling overshot to 1.04 without ImageMagick's composite clamp. The ReadEXR test creates the deprecated plugin with `AllowNonUserCreatablePlugins` and sets the file after creation, which is how the reader infers its layout.
  - **TimeBuffer:** openfx-misc builds TimeBuffer only under `DEBUG` (upstream: "not yet supported by Natron", no sequential render), so the release bundle doesn't ship it. Its alpha change is kept, and its test skips when the plugin is absent. Verified separately against a DEBUG Misc build. It stays out of release.
  - **Follow-up, not fixed:** a plugin with a Source input that also supports the Generator context (TimeBufferRead) is treated as a generator by `OfxEffectInstance::isGenerator`/`getLayerKnobSpec`, and its target-layer picker defaults to `rgba`, which widens alpha-only streams.
