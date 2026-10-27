# M123 - Native Sharpen And Glow

Native, redesigned replacements for the separable CImg filters compositors use every day, plus native versions of the Glow and EdgeBlur PyPlugs:
- **`Sharpen`** is one node. It is an unsharp mask with size, amount and a relative threshold. Soften and Laplacian are modes. It replaces CImg Sharpen, Soften and Laplacian.
- **`Bloom`** is a Kawase-style sum of blurs (ratio, count, filter). It is linear-safe.
- **`Glow`** is native and replaces the Glow PyPlug: threshold, tint, size through the Bloom kernel, and glow-only output. It does not depend on HSVTool.
- **`EdgeExtend`** extends edge colour iteratively and is premult-aware. It is a keying tool.
- **`EdgeBlur`** is native and replaces the EdgeBlur PyPlug. An edge-detected matte drives a blur, reusing the native EdgeDetect and Blur kernels.

Every node follows the native conventions:
- plain ID at major 1;
- scene-linear maths, with no thresholds defined on [0,1];
- host mask, mix and (un)premult from the base class;
- no compatibility with the OFX versions (`DECISIONS/2026-10-12-native-nodes-drop-ofx-legacy.md`).

Every node ships a CPU kernel, a GPU kernel, a native test, a GPU parity test, a cost seed and an icon. It registers through M120 - Native Node Port Kit's per-node fragment files, and retires through one `Engine/OfxRetired/<ofx-plugin-id>` marker per retired ID.

Deps: M120 - Native Node Port Kit (per-node registration, kernel, cost-seed and doc fragments; per-milestone OFX retirement list). M120 - Native Node Port Kit itself depends on M85 - Native GPU Kernels, whose `pixelwrap.slang` and parity harness this milestone uses.

Scouted facts (scouted 2026-10-13 on `main` `338d9324a`; M85 - Native GPU Kernels facts from `origin/milestone/m85-native-gpu-kernels`; re-check at promotion):
- **CImg sources:** `build/assets/plugin-src/openfx-misc/CImg/Blur/CImgBlur.cpp` holds Laplacian, Sharpen, Soften, ChromaBlur, Bloom, ErodeBlur and EdgeExtend.
  - IDs are at `:215-223`, factories at `:3259-3272`, defaults at `:253-357`.
  - Sharpen is input + Laplacian × amount; Soften is input − Laplacian × amount; Laplacian is input − blur. Default size is 3, with Neumann boundary.
  - Bloom is Ratio 2, Count 5, quasi-Gaussian filter. Its widest blur is `size * ratio^(count-1)`.
  - EdgeExtend: Size 10, Slices 5, optional premult in and unpremult out. Each slice blurs, then merges the source over the result.
  - ErodeBlur (`eu.cimg.ErodeBlur`) is a triangle blur plus a linear remap. PIKColor references it (`Gui/Resources/PyPlugs/PIKColor.py:327`), but it is not in the built CImg bundle.
- **Native spatial infrastructure:**
  - `Engine/Nodes/Filter/SpatialFilterSupport.h`: `PlaneJob` (`:125`), `MaskInput` (`:150`), `ProcessedPlanes` (`:178`), `forEachLineChunk` (`:200`), `Boundary {Zero, Nearest}` (`:205`), `WindowLayout` (`:228`) and the templated `writeWindow` (`~:250`), which applies divisor, mask and mix.
  - `Engine/Nodes/Filter/BlurKernels.h`: `Filter` enum (`:59`), `LineFilter::forFilter` (`:130`) and `firGaussian` (`:114`).
  - `Engine/Nodes/Filter/Blur.cpp` is the template node: `initializeKnobs` at `:166`, `addMaskMixKnobs` at `:274`.
- **Blur's GPU path:** `canRenderOnGpu` at `Blur.cpp:894`, `getGpuInputRoI` at `:922`, `renderGpu` at `:936`.
  - It refuses masks, mix ≠ 1 and unprocessed channels at `:897`. This milestone's nodes must not copy that gate. They use M85 - Native GPU Kernels's shared `pixelwrap.slang` finish pass instead.
  - `Engine/Gpu/GpuBlur.h` caps radius at 512 and channels at 4.
  - `blur.slang` (187 lines) has one `blurPass` entry at `:115` with push-constant `BlurParams` (`:9-30`), a shared-memory tile, and Zero or Nearest boundaries.
- **PyPlugs:**
  - `Gui/Resources/PyPlugs/Glow.py` (`fr.inria.Glow`) wires CImgBloom v3 (`:1220`), HSVTool (`:1167`), Grade, Saturation, ColorCorrect, Clamp, Merge, Transform and four Switches.
  - `EdgeBlur.py` (`fr.inria.EdgeBlur`) wraps native EdgeDetect (`:163`), Gamma (`:190`), Blur (`:215`), Shuffle, Merge and Switch.
  - `Tests/PyPlugInstantiate_Test.cpp:84-101` lists both in `kAllBundledPyPlugs` and `kPyPlugsWithColorPinnedInnerNodes`.
- **Other references:**
  - `Engine/OfxEffectInstance.cpp:1311-1329` lists the CImg IDs in the default-channel-set table.
  - `:1354-1356` (`keepColorLayer`) lists CImgChromaBlur and EdgeExtend. Native EdgeExtend reproduces the behaviour: it processes the colour layer by default.
  - `tools/ci/smoke_test.py:305` checks the CImg bundle through `net.sf.cimg.CImgSharpen`.

## Scope

**In:**
- Native `Sharpen` (Sharpen, Soften and Laplacian modes), `Bloom`, `Glow`, `EdgeExtend` and `EdgeBlur`. Each has CPU and GPU kernels, a native test and a GPU parity test.
- Retirement of CImg Sharpen, Soften, Laplacian, Bloom and EdgeExtend from the default bundle.
- Removal of the Glow and EdgeBlur PyPlugs.

**Out:**
- ChromaBlur, ErodeSmooth, SharpenShock, SharpenInvDiff, Smooth, Denoise, Equalize and HistEQ stay on OFX. No milestone owns them.
- Median, Bilateral, Guided and DirBlur: M130 - Native Edge-Preserving And Directional Filters.
- Defocus-family blurs: M131 - Native Defocus Family.
- Renaming existing native nodes' IDs: M111 - Native Node Identity Cleanup.

**Edge cases:**
- **Sharpen threshold:** relative to the local blurred value (`|in − blur| / max(|blur|, ε)`). Detail is then judged the same at 0.1 and at 10.
- **Negative values:** pass through every node untouched. Nothing clamps.
- **Bloom at large count:** the widest blur can exceed the GPU cap. Above `kGpuBlurMaxRadius`, Bloom downsamples the input pyramid, the way Kawase bloom is meant to work. It does not fall back to the CPU.
- **EdgeExtend on unpremultiplied input:** the premult-in step is a knob, on by default, so straight-alpha plates still extend correctly.

**Assumptions:**
- Unconfirmed: PIKColor's stale `eu.cimg.ErodeBlur` reference is retargeted to native ErodeDilate followed by Blur.
- Unconfirmed: Glow keeps the PyPlug's user-facing knob set minus HSVTool's controls. Its threshold is a luminance threshold with soft knee, in scene-linear units.
- Unconfirmed: the M85 - Native GPU Kernels shared finish pass (`pixelwrap.slang`) covers mask, mix and process channels for spatial nodes. M85 - Native GPU Kernels says spatial nodes get divide and finish passes from it.

## Phase 122.1: Sharpen

- [x] M123.P1.T1 — Write the native Sharpen node (CPU) with Sharpen, Soften and Laplacian modes
  - files: `Engine/Nodes/Filter/Sharpen.h`, `Engine/Nodes/Filter/Sharpen.cpp`, `Engine/Nodes/Filter/Sharpen.reg.inc`
  - approach:
    - Copy `Blur.cpp`'s structure: plane jobs from `SpatialFilterSupport.h`, a blur through `LineFilter::forFilter`, then a per-pixel combine.
    - Knobs: `mode` (Sharpen, Soften, Laplacian), `size` (default 3), `filter` (Gaussian, the FIR default from M88 - FIR Gaussian Blur), `amount` (default 1), `threshold` (relative, default 0, i.e. off).
    - Sharpen: `out = in + amount * w * (in − blur)`. Soften: `out = in − amount * w * (in − blur)`. Laplacian: `out = amount * (in − blur)`. `w` is the soft threshold weight from the relative detail measure in `## Scope`.
    - No clamps. Host mask, mix and premult come from the base.
  - verify: at the phase build, `ctest -R NativeSharpen` (written in T2) passes.
  - size: M

- [x] M123.P1.T2 — Test Sharpen on CPU
  - files: `Tests/Native/NativeSharpen_Test.cpp`
  - approach:
    - Use a step edge and an HDR plate (values 0.01..50) as fixtures.
    - Assert the following:
      - Amount 0 is identity.
      - Laplacian of a constant is 0.
      - Sharpen = in + Laplacian at amount 1 and threshold 0.
      - The threshold behaves the same at a scale of 1× and 100×.
      - Negatives pass through.
    - Record regression fixtures.
  - verify: at the phase build, `ctest -R NativeSharpen` passes.
  - size: M

- [x] M123.P1.T3 — Sharpen on the GPU
  - files: `Engine/Gpu/Kernels/sharpen.slang`, `Engine/Gpu/Kernels/sharpen.cmake`, `Engine/Nodes/Filter/Sharpen.cpp`, `Tests/Native/NativeSharpenGpu_Test.cpp`
  - approach:
    - Reuse `blur.slang`'s separable passes for the blur, then a combine kernel built on M85 - Native GPU Kernels's `pixelwrap.slang` finish pass.
    - `canRenderOnGpu` accepts masks, mix and channel subsets.
    - Add the cost seed as the node's `.costseed.inc` fragment.
  - verify: at the phase build, `ctest -R NativeSharpenGpu` passes on RADV within 1e-5 absolute.
  - size: M

## Phase 122.2: Bloom and Glow

- [x] M123.P2.T1 — Write the native Bloom node (CPU)
  - files: `Engine/Nodes/Filter/Bloom.h`, `Engine/Nodes/Filter/Bloom.cpp`, `Engine/Nodes/Filter/Bloom.reg.inc`
  - approach:
    - Sum `count` blurs of sizes `size * ratio^i`, normalised by the count, with knobs for `size`, `ratio` (2), `count` (5) and `filter`.
    - Each level blurs a 2× downsampled copy of the previous one, so the cost stays bounded at large counts. Downsampling and upsampling go through the existing line filters.
    - Scene-linear, with no clamps.
  - verify: at the phase build, `ctest -R NativeBloom` passes.
  - size: L

- [x] M123.P2.T2 — Test Bloom on CPU
  - files: `Tests/Native/NativeBloom_Test.cpp`
  - approach:
    - A constant is preserved, since the sum is normalised.
    - Energy is conserved within tolerance on a point source.
    - Count 1 equals a plain Blur of `size`.
    - Record regression fixtures.
  - verify: at the phase build, `ctest -R NativeBloom` passes.
  - size: M

- [x] M123.P2.T3 — Bloom on the GPU
  - files: `Engine/Gpu/Kernels/bloom.slang`, `Engine/Gpu/Kernels/bloom.cmake`, `Engine/Nodes/Filter/Bloom.cpp`, `Tests/Native/NativeBloomGpu_Test.cpp`
  - approach:
    - Use downsample and upsample kernels plus `blur.slang` passes per level.
    - Allocate the pyramid from M84 - GPU Placement And Residency's residency pools, so a Bloom joins a resident region.
  - verify: at the phase build, `ctest -R NativeBloomGpu` passes on RADV within 1e-4 relative.
  - size: L

- [x] M123.P2.T4 — Write the native Glow node on the Bloom kernel (CPU and GPU)
  - files: `Engine/Nodes/Filter/Glow.h`, `Engine/Nodes/Filter/Glow.cpp`, `Engine/Gpu/Kernels/glow.slang`, `Engine/Nodes/Filter/Glow.reg.inc`, `Tests/Native/NativeGlow_Test.cpp`
  - approach:
    - The node runs three steps in order:
      1. Threshold the source: luminance above `threshold` with a soft knee. Luma weights come from the project working space via `ColorMath::luminance`.
      2. Tint the result (`tint` colour × `brightness`).
      3. Bloom it with Bloom's kernel and add it back over the source.
    - `glowOnly` outputs the glow alone.
    - Expose Bloom's `size`, `ratio`, `count` and `filter` knobs.
    - The GPU path is the threshold and tint point op, then Bloom's GPU levels, then an add.
  - verify: at the phase build, the following pass:
    - `ctest -R NativeGlow`: threshold above the image max gives identity; glowOnly plus the source equals the full output.
    - The test's GPU parity case on RADV, within 1e-4 relative.
  - size: L

- [x] M123.P2.T5 — Remove the Glow PyPlug and update its tests
  - files: `Gui/Resources/PyPlugs/Glow.py`, `Gui/Resources/PyPlugs/Glow.png`, `Gui/GuiResources.qrc`, `Tests/PyPlugInstantiate_Test.cpp`
  - approach:
    - Delete the PyPlug and its resources.
    - Remove `fr.inria.Glow` from `kAllBundledPyPlugs` and `kPyPlugsWithColorPinnedInnerNodes` (`PyPlugInstantiate_Test.cpp:84-101`).
    - Grep for any other `fr.inria.Glow` use.
  - verify: at the phase build, `ctest -R PyPlugInstantiate` passes and the tab menu offers only the native Glow.
  - size: M

## Phase 122.3: EdgeExtend and EdgeBlur

- [x] M123.P3.T1 — Write the native EdgeExtend node (CPU)
  - files: `Engine/Nodes/Filter/EdgeExtend.h`, `Engine/Nodes/Filter/EdgeExtend.cpp`, `Engine/Nodes/Filter/EdgeExtend.reg.inc`, `Tests/Native/NativeEdgeExtend_Test.cpp`
  - approach:
    - Knobs: `size` (10), `slices` (5), `premultiplyInput` (on), `unpremultiplyOutput` (on).
    - Each slice blurs the premultiplied colour and alpha at a growing size, divides colour by alpha where alpha > ε, and merges the source over the result.
    - It processes the colour layer by default, matching the `keepColorLayer` behaviour.
    - Test the following:
      - Opaque interior pixels are unchanged.
      - Pixels just outside a matte edge take the edge colour.
      - Alpha is untouched when unpremult out is on.
  - verify: at the phase build, `ctest -R NativeEdgeExtend` passes.
  - size: M

- [x] M123.P3.T2 — EdgeExtend on the GPU
  - files: `Engine/Gpu/Kernels/edgeextend.slang`, `Engine/Gpu/Kernels/edgeextend.cmake`, `Engine/Nodes/Filter/EdgeExtend.cpp`, `Tests/Native/NativeEdgeExtendGpu_Test.cpp`
  - approach: run `blur.slang` passes per slice, plus a divide-and-over combine kernel per slice, keeping the iterations resident.
  - verify: at the phase build, `ctest -R NativeEdgeExtendGpu` passes on RADV within 1e-4 relative.
  - size: M

- [x] M123.P3.T3 — Write the native EdgeBlur node (CPU and GPU)
  - files: `Engine/Nodes/Filter/EdgeBlur.h`, `Engine/Nodes/Filter/EdgeBlur.cpp`, `Engine/Nodes/Filter/EdgeBlur.reg.inc`, `Tests/Native/NativeEdgeBlur_Test.cpp`
  - approach:
    - It has an optional external matte input, replacing the PyPlug's `externalMatte` switch.
    - The edge matte comes from the native EdgeDetect kernel on the chosen channel (alpha by default). The matte is widened by `edgeSize` and softened.
    - The source is blurred by Blur's kernel with `size`, then mixed in through the edge matte.
    - Call EdgeDetect's and Blur's kernel functions directly rather than building an internal graph. Do the same on the GPU, with a mix-by-matte combine pass.
  - verify: at the phase build, the following pass:
    - `ctest -R NativeEdgeBlur`: a flat matte gives identity; the blur is confined to edges.
    - The test's GPU parity case, within 1e-4 relative.
  - size: L

- [x] M123.P3.T4 — Remove the EdgeBlur PyPlug and update its tests
  - files: `Gui/Resources/PyPlugs/EdgeBlur.py`, `Gui/Resources/PyPlugs/EdgeBlur.png`, `Gui/GuiResources.qrc`, `Tests/PyPlugInstantiate_Test.cpp`
  - approach: delete the PyPlug and its resources, and remove `fr.inria.EdgeBlur` from the PyPlug test lists. The test edits sit next to P2.T5's, so rebase if P2.T5 has landed.
  - verify: at the phase build, `ctest -R PyPlugInstantiate` passes.
  - size: M

## Phase 122.4: Retire the OFX versions

- [x] M123.P4.T1 — Retire CImg Sharpen, Soften, Laplacian, Bloom and EdgeExtend and fix their references
  - files: `Engine/OfxRetired/<ofx-plugin-id>` (five markers), `Engine/OfxEffectInstance.cpp`, `tools/ci/smoke_test.py`, `Gui/Resources/PyPlugs/PIKColor.py`
  - approach:
    - Add an `Engine/OfxRetired/<ofx-plugin-id>` marker for each of the five IDs.
    - Drop them from the default-channel-set table (`OfxEffectInstance.cpp:1311-1329`) and from `keepColorLayer` (`:1354-1356`), keeping ChromaBlur there.
    - Switch the smoke test's CImg bundle check (`smoke_test.py:305`) to a CImg plug-in that stays, e.g. `net.sf.cimg.CImgDenoise`.
    - Retarget PIKColor's `eu.cimg.ErodeBlur` (`:327`) to native ErodeDilate followed by Blur.
  - verify: at the phase build, the following hold:
    - `ctest` passes.
    - The smoke test passes.
    - Loading a project that names a retired ID shows the missing-plugin node.
    - The PIKColor PyPlug instantiates.
  - size: M

- [x] M123.P4.T2 — Document the nodes
  - files: `Engine/Nodes/Filter/docs/{Sharpen,Bloom,Glow,EdgeExtend,EdgeBlur}.md`
  - approach: document each node: knobs, maths, GPU path and the parity tolerance, one file per node (M120 - Native Node Port Kit's scheme).
  - verify: each doc names every knob that exists in the node code (reviewed at the gate).
  - size: S

## Phase 122.5: UAT round 1

- [x] M123.P5.T1 — EdgeExtend inputs, threshold and order
  - files: `Engine/Nodes/Filter/EdgeExtend.{h,cpp}`, tests, docs
  - approach: make the premultiplied/unpremultiply input handling behave as documented; add a threshold (0–1 on alpha) where the extend begins; put the premultiplied control above Size.
  - verify: gtests for premultiplied and straight inputs and for the threshold; the GUI check.
  - size: M
- [x] M123.P5.T2 — Glow mix, no "(Un)premult by" on filters
  - files: `Engine/Nodes/Filter/{Glow,Bloom}.{h,cpp}`
  - approach: Glow gets the standard Mix knob; Glow and Bloom drop the "(Un)premult by" knob, as filtering nodes do.
  - verify: gtests; knob list check.
  - size: M
- [x] M123.P5.T3 — EdgeBlur through IBlur
  - files: `Engine/Nodes/Filter/EdgeBlur.{h,cpp}`, `IBlur`
  - approach: blur with M184's variable-size IBlur driven by the edge distance, so the result has no ghosting from blended slices; CPU and GPU.
  - verify: gtests; goldens re-recorded.
  - size: L

## Decisions

- 2026-10-26 — **UAT round 1 (user):** EdgeExtend's premultiply/unpremultiply inputs don't work as described; it needs a threshold on alpha (0–1) where the extend begins, and the premultiply control above Size. Glow lacks Mix. Filtering nodes (Glow, Bloom) don't get the "(Un)premult by" knob. EdgeBlur uses IBlur to avoid ghosting.
- 2026-10-25 — **Glow ports every PyPlug control (bosley-native-nodes):** rotate (the stretch turns clockwise; non-axis angles bloom in a turned frame resampled from absolute positions so tiles match the whole frame), hue isolation (HSVTool's key), and the mask multiplying the highlight map as the PyPlug did (Glow has no mix or mask-invert, as before). The mask applies in image space rather than the PyPlug's unrotated turned frame. A recursive (IIR) filter under a non-axis rotation still depends on where the turned frame starts, so only the FIR and box filters are tile-exact there. Bloom repeats the edge pixel outside the image while Glow treats it as black; Bloom's size defaults to 0 (identity).
- 2026-10-25 — **Bloom spike 2 (GPU) results (bosley-native-nodes):** on the RX 7900 XTX (RADV) every GPU variant matches its CPU reference within 4e-7 relative (pass bar 1e-4). Per frame, 4 channels, count 5: the full-resolution line IIR costs 23 ms at HD and 55 ms at UHD at any size; the pyramid costs 0.45-1.4 ms at HD and 2.7-5.3 ms at UHD, flat in size. [1 3 3 1] + bicubic, the CPU accuracy winner, is 1.3 ms HD / 5.2 ms UHD; box + bilinear is the fastest but the least accurate. The pyramid with [1 3 3 1] downsample and bicubic upsample is the GPU-resident path; it is 10-40 times cheaper than the IIR and the closer match to the FIR default.
- 2026-10-25 — **Bloom spike 1 (CPU) results (bosley-native-nodes):** a decimated pyramid with a [1 3 3 1] downsample and bicubic upsample stays within 4e-4 to 3e-3 RMS (relative to peak) of the full-resolution FIR sum on an HDR plate at sizes 3 to 400, count 5, and tiles are bit-exact across windows (seam test exact for every variant). The full-resolution IIR sum (the CPU-fallback-free alternative) differs from the FIR sum by 6e-4 to 4e-2 RMS, ten times more, because the recursive filter is a different kernel; with FIR the default, the pyramid is the closer match. Box downsampling is markedly worse (up to 1e-2 RMS). The spike's CPU timings are not representative of an optimised pyramid. Spike 2 (GPU cost on the RX 7900 XTX) decides between them.
- 2026-10-25 — **EdgeExtend with no active slice is an identity (bosley-native-nodes):** at sizes where every slice's sigma falls below the FIR minimum, both paths pass the source through, including the premultiply round trip; output can step slightly as a slice crosses that threshold.
- 2026-10-25 — **Bloom default filter FIR Gaussian (user).**
- 2026-10-25 — **Bloom keeps GPU residency (user):** no CPU fallback for large Bloom sizes if at all possible, so option A (full-resolution FIR sum falling back to the CPU over 512 px) is out. If no option is a clear winner, spike the candidates (pyramid in PassExecutor, a custom op, other GPU-resident schemes) and measure before choosing.
- 2026-10-25 — **EdgeBlur's external matte waits on M184 (bosley-native-nodes):** a PassExecutor op reads one source plus a mask; M184 (Variable-Size Filter Kit) already extends the pass runner with views of extra (Aux) inputs on CPU and GPU (IBlur's Map), which is exactly what EdgeBlur's Matte input needs. P3.T3 is built on that once M184 merges, rather than as a Custom op or a second framework change. EdgeExtend follows CImg: each slice merges the running result over its blur and the divide happens once at the end.
- 2026-10-25 — **Sharpen calls (bosley-native-nodes):** plain ID "Sharpen"; the threshold is relative (detail/value), weight ramping linearly from 0 at t to 1 at 2t (epsilon 1e-6), and Laplacian ignores it; nearest-pixel edges and no output growth, as Blur. GPU parity holds FIR filters to 1e-5 absolute and the recursive/box filters to Blur's 1e-4 IIR tolerance, on the [0,1] parity source. Filters are multi-pass ops through PassExecutor (SpatialFilterSupport.h is gone); the filters README needs a note that Sharpen uses the Premult stage.
- 2026-10-13 — **Family in scope (user, /cat-plan grill):** Sharpen, Bloom with a native Glow, EdgeExtend and a native EdgeBlur. ChromaBlur and the expensive CImg PDE filters stay on OFX.
- 2026-10-13 — **Redesign, not port (user):** Nuke-like knob sets, scene-linear maths, plain IDs, siblings consolidated. Sharpen absorbs Soften and Laplacian as modes.
- 2026-10-13 — **GPU ships with each port (user):** every node has a GPU kernel and a parity test in this milestone, after M85 - Native GPU Kernels.

**Verification gate:** the full build and `ctest` pass. Every `Native*Gpu` parity test in this milestone passes on RADV. Under Xvfb, Sharpen, Bloom, Glow, EdgeExtend and EdgeBlur are created from the tab menu, rendered with a mask and mix ≠ 1, and screenshotted, and the screenshots are shared with the user before sign-off. The retired CImg IDs load as missing-plugin nodes. An AppImage built with `package.sh` is handed over for UAT.
Review round (Claude, PR #116): Bloom overflow caps and Glow's per-channel grade fixed at 2084280fa; release gate green. Cost seeds pending; UAT pending.

2026-10-26: UAT round 1 fixes (0a4b160cf) and goldens; release green. AppImage Natron-m123-sharpen-glow.AppImage for UAT round 2.

UAT round 2 approved by the user 2026-10-26; follow-up M238 - EdgeExtend Options. Merges after M184 - Variable-Size Filter Kit (RADV parity pending).

2026-10-27: M184 - Variable-Size Filter Kit landed (#113) with rewritten history; PR #116 closed with its base. The milestone's own commits were re-applied on main and adapted to M184's final APIs (per-frame metadata, Map reads as a remapped black pixel outside its image); new PR #129. UAT stays approved; goldens re-recorded as CI requires.
