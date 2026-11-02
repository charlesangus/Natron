# M286 - Utility Renderer Deep Output

Render deep images from Natron Utility in-process, and settle the host-side deep transport
every Hydra delegate will use: a delegate's deep result leaves through a custom render buffer
resource that USDRender turns into a `DeepImage` — no file, no extra process. Raw per-sample
fragments are compacted by a shared deep compaction library (modelled on DeepThinner's
controls, trimmed to four) at render time, and the same library ships as a standalone
`DeepCompact` node for deep from any source.

## Scope

**In:**
- Deep fragments along each camera sample's primary ray: every surface hit with partial opacity (or ior = 1 transparency) records a fragment and the ray continues; an opaque or refractive surface records its fragment (refracted and reflected light baked into its colour) and ends the ray.
- Fragment channels: z (front; zBack = z for surfaces), premultiplied colour and alpha weighted by the pixel filter, plus every bound AOV from M285 - Utility Renderer AOVs that is meaningful per sample (data AOVs, lobes, light groups; Cryptomatte excluded — the ids are the deep sample's own `primId`).
- Per-pixel working bins on both backends: a fragment merges into an existing bin whose depth is within `Merge Distance`, otherwise opens a new bin; when the bins are full, the two closest adjacent bins merge.
- Shared compaction library `DeepCompaction` (no Hydra types) with four controls: `Merge Distance` (scene units, default 0.1), `Max Samples` per pixel (default 0 = unlimited), `Alpha Cull` threshold (default 0), `Occlusion Cutoff` (accumulated alpha, default 1.0 = off). Over budget, the closest adjacent pair merges first; samples behind the occlusion cutoff are dropped before merging.
- The host transport: `HdRenderBuffer::GetResource` returns a deep-buffer handle (sample counts, offsets, named channels) that HydraRenderImpl recognises for any delegate and converts into `DeepImage`; USDRender gains deep output.
- `DeepCompact` node (Engine/Nodes/Deep) exposing the same four controls on any deep input.

**Out:**
- Cycles and MoonRay filling the deep buffer — M257 - Cycles Deep Output and M258 - MoonRay Deep Output (now scoped by this transport).
- Volumes and volumetric zBack ranges — none.
- DeepThinner's contribution cull, volumetric collapse, colour-similarity merge and depth-range clip — none (user, 2026-11-01: "probably don't need all that").
- Deep Cryptomatte — none.

**Edge cases:**
- Pixel with no hits: zero samples.
- Fragments from different prims within `Merge Distance` merge (the user's rule is depth-only); the merged sample keeps the id of the higher-coverage contributor.
- Motion blur: fragments from different time buckets merge by depth like any others.
- `Max Samples` 1: the pixel collapses to one sample equal to the flattened result at the front depth.
- Accuracy: with Merge Distance 0, Max Samples unlimited and both culls off, the flattened deep image equals the flat render within 1e-4.
- Bins full on the GPU before compaction (fixed working capacity, setting `Working Samples`, default 32): closest adjacent pair merges in-kernel.

**Assumptions:**
- `HdRenderBuffer::GetResource(bool)` returning a `VtValue` that wraps a Natron-defined handle crosses the delegate/host boundary without RTTI issues since the delegate is compiled into a Natron library.
- USDRender can declare deep as its output kind by a knob on the typed-edge framework (M17 - Typed Edges Framework); if output kind is fixed per node, the deep render becomes a second output — confirm in `EffectInstance::producesDeepData()` callers.

## Phase 286.1: Compaction library and node

- [ ] M286.P1.T1 — DeepCompaction library
  - files: `Engine/DeepCompaction.{h,cpp}` (new), `Engine/CMakeLists.txt`, `Tests/DeepCompaction_Test.cpp`
  - approach: operate on one pixel's sample run (front-to-back sorted SoA view over `DeepImage`'s channels): alpha cull → occlusion cutoff → merge within distance → enforce max samples by repeatedly merging the closest adjacent pair (premultiplied channels add; alpha composites as `a + b(1−a)` with colours combined over-style for non-coincident samples — document the exact merge rule in the header). Thread-local scratch, no allocation per pixel.
  - verify: gtest — with all controls off the flattened result is unchanged bit-for-bit; a 20-sample pixel compacted to 5 flattens within 1e-4 of the original; samples behind accumulated alpha 0.999 are dropped with the cutoff at 0.999; merge distance 0.1 joins samples at z 1.00 and 1.05 but not 1.2.
  - size: L

- [ ] M286.P1.T2 — DeepCompact node
  - files: `Engine/Nodes/Deep/DeepCompact.{h,cpp,reg.inc}`, `Engine/AppManager.cpp`, `Engine/Nodes/Deep/docs/DeepCompact.md`, `Tests/DeepCompact_Test.cpp`
  - approach: native deep node over `DeepCompaction`; knobs `mergeDistance` ("Merge Distance"), `maxSamples` ("Max Samples"), `alphaCull` ("Alpha Cull"), `occlusionCutoff` ("Occlusion Cutoff"); passes every channel through; info text reports samples in/out for the last render.
  - verify: gtest — DeepRead of a fixture → DeepCompact (Max Samples 4) → every pixel has ≤4 samples and DeepToImage matches the uncompacted flatten within tolerance.
  - size: M

## Phase 286.2: Deep from Natron Utility

- [ ] M286.P2.T1 — Deep fragments and working bins in the wavefront
  - files: `Engine/Nodes/Scene/Utility/Kernels/DeepBins.slang`, `Engine/Nodes/Scene/Utility/Kernels/ShadeSurface.slang`, `Engine/Nodes/Scene/Utility/UtilityRenderPass.cpp`, `Engine/Nodes/Scene/Utility/WavefrontGpu.cpp`, `Tests/Scene/NatronUtilityDeep_Test.cpp`
  - approach: when a deep AOV is bound, primary-ray shading emits a fragment per hit and continues through partial-opacity/ior-1 surfaces (a continuation in the bounce queue tagged primary-deep); fragments insert into fixed per-pixel bins (Working Samples) with merge-within-distance and closest-pair overflow, in shared Slang on both backends.
  - verify: gtest — two 50%-opaque parallel cards give two samples per covered pixel at their depths; an opaque card behind ends the ray with a third sample; CPU and GPU bins agree within tolerance.
  - size: L

- [ ] M286.P2.T2 — Deep render buffer and final compaction
  - files: `Engine/Nodes/Scene/Utility/UtilityDeepBuffer.{h,cpp}`, `Engine/Nodes/Scene/Utility/UtilityRenderDelegate.cpp`, `Engine/Nodes/Scene/HydraDeepBuffer.h` (new, the delegate-neutral handle), `Tests/Scene/NatronUtilityDeep_Test.cpp`
  - approach: a `deep` AOV served by a render buffer subclass whose `Resolve()` sorts bins front-to-back and runs `DeepCompaction` with the four delegate settings, producing counts/offsets/channels; `GetResource` wraps them in the `HydraDeepBuffer` handle defined host-side so any delegate can produce it.
  - verify: gtest — the resolved buffer's flatten equals the flat `color` render within 1e-4 with compaction off; Max Samples 2 caps every pixel.
  - size: L

- [ ] M286.P2.T3 — USDRender deep output
  - files: `Engine/Nodes/Scene/USDRender.{h,cpp}`, `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/docs/USDRender.md`, `Tests/Scene/USDRender_Test.cpp`
  - approach: a deep output on USDRender (per the typed-edge framework) that requests the `deep` AOV plus the selected per-sample AOVs, converts the `HydraDeepBuffer` into a `DeepImage` with layers named as the flat AOVs, in `renderDeep`. A delegate that can't produce deep gives a node error naming it.
  - verify: gtest — USDRender (Natron Utility, deep) → DeepToImage equals the flat USDRender within tolerance; → DeepMerge with a DeepFromImage card holds out correctly; Storm with deep requested gives the named error.
  - size: L

**Verification gate:** full build and ctest green on both backends; a deep render of overlapping semi-transparent and opaque objects is merged with a 2D card via DeepMerge in the GUI under Xvfb and the screenshot is shared with the user.

## Decisions

- 2026-11-01 — **The transport is settled here, for every delegate (user):** a custom render-buffer resource converted host-side into `DeepImage`; M255 - Delegate Output Spikes' deep spike only asks whether Cycles and MoonRay can fill it.
- 2026-11-01 — **User-controlled compaction modelled on DeepThinner (user):** at minimum a merge distance in scene units and a per-pixel sample budget; Alpha Cull and Occlusion Cutoff added from DeepThinner's defaults-on passes.
- 2026-11-01 — **DeepCompact node in this milestone (user).**
- 2026-11-01 — **Refractive surfaces end the deep ray** with refracted light baked into their colour; only partial opacity and ior-1 transparency continue it.
