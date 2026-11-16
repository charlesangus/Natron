# M255.P2.T3 — Draft task lists for M256, M257, M258

Each task is in PLAN-FORMAT §6 form, ready for `/cat-plan` to paste. The bracketed user decisions refer to `REPORT-P2T3-findings.md` §3.

## M256 - Delegate AOV Selection

## Phase 256.1: RenderVars to AOVs

- [ ] M256.P1.T1 — Read RenderVars into AOV requests
  - files: `Engine/Nodes/Scene/RenderSettingsPrim.{h,cpp}`, `Tests/Scene/RenderSettingsPrim_Test.cpp`
  - approach: `aovRequests(prim, renderer, time)` uses `UsdRenderComputeSpec(activePrim, {renderer ns, "driver", "natron"})` and returns the union of all products' vars, deduplicated by var path; products that disagree on camera or resolution are an error. raw sourceName is verbatim, primvar becomes `primvars:<name>`, lpe becomes `lpe:<expr>`, intrinsic is rejected. Each request carries dataType, `natron:layer` and the namespaced attributes.
  - verify: gtest on in-memory .usda: two overlapping products give a deduplicated union; each sourceType maps as stated; intrinsic is rejected; two cameras error; namespaced attributes survive.
  - size: M
- [ ] M256.P1.T2 — AOV-to-layer naming table
  - files: `Engine/Nodes/Scene/AovLayers.{h,cpp}` (new), `Engine/Nodes/Scene/USDRender.cpp` (:67-84), `Engine/CMakeLists.txt`, `Tests/Scene/AovLayers_Test.cpp`
  - approach: layer = AOV name without its namespace, or `natron:layer`. Channels: colour R,G,B(,A); normal/position/motion X,Y,Z(,W); uv U,V,W; scalars one channel. The kind comes from dataType, else a per-delegate table, else the HdFormat channel count; a dataType that disagrees with the format warns.
  - verify: gtest: `cycles:diffuse_direct` gives diffuse_direct.RGB, `primvars:st` gives st.UVW, depth gives depth.Z, `natron:layer` overrides; existing layer names are unchanged.
  - size: M
- [ ] M256.P1.T3 — HydraRenderImpl requests the spec's AOVs
  - files: `Engine/Nodes/Scene/HydraRenderImpl.{h,cpp}` (:1397-1413, :3038), `Tests/Scene/USDRender_Test.cpp`
  - approach: color, depth, primId and instanceId plus the spec's requests. A null buffer after `SetRendererAovs` means rejected, reported by RenderVar path; elementId is treated as rejected. Any HdFormat is read into its layer, and only requested AOVs are bound.
  - verify: gtest on Cycles: RenderVars depth, normal and `primvars:nope` give two layers and an error naming `/Render/product/nope`; with no vars the render is unchanged.
  - size: L
- [ ] M256.P1.T4 — USDRender layers and hash follow the spec
  - files: `Engine/Nodes/Scene/USDRender.{h,cpp}` (:496-508, render loop, appendNodeContentHash), `Tests/Scene/USDRender_Test.cpp`
  - approach: produced layers = the active prim's spec layers, cached per time. Edits to the RenderProduct and RenderVar children change the hash through M253.P2.T8's side table.
  - verify: gtest: adding a RenderVar adds the layer and invalidates; an unrelated edit does not; the channel selector lists the new layer.
  - size: M
- [ ] M256.P1.T5 — Per-AOV settings in, delegate metadata out
  - files: `Engine/Nodes/Scene/HydraRenderImpl.{h,cpp}` (engine subclass), `Engine/Nodes/Scene/USDRender.cpp`, `Tests/Scene/USDRenderMoonRay_Test.cpp`, `Tests/Scene/USDRender_Test.cpp`
  - approach: a `UsdImagingGLEngine` subclass calls `SetRenderOutputSettings` with each var's `driver:parameters:aov:*` as `aovSettings`. `aovMetadata:<aov>` render stats become layer metadata (for M285's Cryptomatte manifest).
  - verify: gtest: a MoonRay var's `driver:parameters:aov:moonray:*` reaches hdMoonray; a test stat dictionary appears as layer metadata.
  - size: L

## Phase 256.2: Checklist authoring

- [ ] M256.P2.T1 — Delegate AOV catalogue
  - files: `Engine/Nodes/Scene/DelegateSupport.{h,cpp}`, `Engine/Nodes/Scene/CyclesAovs.cpp`, `Engine/Nodes/Scene/MoonRayAovs.cpp`, `Tests/Scene/USDRenderCycles_Test.cpp`, `Tests/Scene/MoonRayAovs_Test.cpp`
  - approach: `availableAovs(renderer)` returns the delegate's `aovNames` stat when published, else a Natron-side table per delegate.
  - verify: gtest: Cycles lists its 5 (all `cycles:<pass>` after 256.3); MoonRay lists its table; Storm lists its own.
  - size: M
- [ ] M256.P2.T2 — USDRenderSettings writes RenderProduct and RenderVars
  - files: `Engine/Nodes/Scene/USDRenderSettings.{h,cpp}` (:342-419), `Engine/Nodes/Scene/docs/USDRenderSettings.md`, `Tests/Scene/USDRenderSettings_Test.cpp`
  - approach: an AOV checklist from P2.T1, plus a `aovs` KnobTable "Custom AOVs" (Name, Source Type, Data Type, Layer). It writes a `product` RenderProduct, `orderedVars` and one RenderVar per pick; unpicking removes the var.
  - verify: gtest: normal plus a `primvars:st` row gives two correct RenderVars; USDWrite round-trips them; USDRender renders both layers.
  - size: L
- [ ] M256.P2.T3 — Hand-written RenderVars from a file
  - files: `Tests/Scene/USDRenderSettings_Test.cpp`, `Tests/fixtures/scene/rendervars.usda` (new, small)
  - approach: USDRead loads a product with three vars and a renamed `natron:layer`; USDRender renders it with no USDRenderSettings node.
  - verify: the gtest sees the three layers under their authored names.
  - size: M

## Phase 256.3: Cycles fork AOVs (user decision 3)

- [ ] M256.P3.T1 — `cycles:<pass>`, `aovNames`, drop elementId
  - files: charlesangus/cycles `natron/deep-5.2` (`src/hydra/render_delegate.cpp` :385, :416-439, :434; `src/hydra/session.cpp` :21-28, :149-178), `tools/ci/local/Dockerfile`
  - approach: patch steps 1, 2 and 6 from P1.T1; a pass's format follows its channel count. Bump the pin, publish the image and recreate natron-dev.
  - verify: the P1.T1 probe gets non-null `cycles:diffuse_direct`/`ao`/`mist`, the stat lists the passes, and no elementId warning appears.
  - size: M
- [ ] M256.P3.T2 — Cycles pass kinds in Natron
  - files: `Engine/Nodes/Scene/CyclesAovs.{h,cpp}`, `Engine/Nodes/Scene/AovLayers.cpp`, `Tests/Scene/USDRenderCycles_Test.cpp`
  - approach: a table of Cycles pass names to kind, used by P1.T2.
  - verify: gtest: diffuse_direct gives RGB, uv gives UVW, mist gives one channel; the light passes sum to beauty with denoising off.
  - size: M
- [ ] M256.P3.T3 — Light groups and primvar AOVs in the Cycles fork
  - files: charlesangus/cycles (`src/hydra/light.cpp` :370-398, `material.cpp` :578-589/742/766, `session.cpp` :74-85), `tools/ci/local/Dockerfile`, `Tests/Scene/USDRenderCycles_Test.cpp`
  - approach: `cycles:lightgroup:<name>` from an attribute on lights, meshes and the background; `primvars:<name>` built like the instanceId AOV; materials re-sync when the requested set changes.
  - verify: gtest: two light groups sum to the direct lighting; `primvars:st` matches an authored ramp.
  - size: L

**Verification gate:** build and ctest green on the new Cycles image; under Xvfb, three Cycles passes ticked plus one custom primvar var give four layers in the viewer and in one EXR; screenshots shared with the user.

Notes: stack M257's fork commits on 256.3's. M285 depends on P1.T5.

## M257 - Cycles Deep Output (depends on M286 - Utility Renderer Deep Output)

## Phase 257.1: Transport

- [ ] M257.P1.T1 — Land the deep hand-off against M286's header (needs M286.P2.T2)
  - files: charlesangus/cycles (`src/integrator/path_trace.cpp`, `src/hydra/output_driver.cpp`, `session.{h,cpp}`, `render_delegate.cpp`, vendored `HydraDeepBuffer.h`), `Engine/Nodes/Scene/HydraDeepBuffer.h` (layout version), `tools/ci/local/Dockerfile`, `tools/ci/local/build-cycles.sh`
  - approach: port `spike/m255/deep/cycles-deep.patch` with M286's header vendored byte-identical. A `kLayoutVersion` is checked host-side, and a mismatch is a node error. Bump the pin.
  - verify: the P2.T1 probe on the new image returns the buffer with the matching version; the colour AOV is unchanged.
  - size: L
- [ ] M257.P1.T2 — USDRender deep on Cycles (needs M286.P2.T3)
  - files: `Engine/Nodes/Scene/CyclesAovs.{h,cpp}`, `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Tests/Scene/USDRenderCycles_Test.cpp`
  - approach: bind `cycles:deep`; `deep_merge_threshold` from Merge Distance; host `DeepCompaction` (user decision 6).
  - verify: gtest: cards and a sphere give a DeepImage with R G B A Z ZBack; Max Samples 2 caps every pixel.
  - size: M

## Phase 257.2: Production fixes in the fork

- [ ] M257.P2.T1 — Bounded kernel memory
  - files: charlesangus/cycles (`src/kernel/film/deep_write.h`, `src/session/deep_buffers.h`, `src/scene/film.cpp`), `Engine/Nodes/Scene/CyclesSettings.cpp`, `Tests/Scene/USDRenderCycles_Test.cpp`
  - approach: `deep_max_samples` default 16, exposed as "Deep Working Samples"; a full pixel merges into the nearest slot; an allocation failure is a node error.
  - verify: gtest: deep allocation at 256x256 is ≤ 16×32 B/px; a pixel with 40 overlapping cards flattens within 1e-3.
  - size: M
- [ ] M257.P2.T2 — Flatten correctness and flat pixels
  - files: charlesangus/cycles (`src/hydra/output_driver.cpp`, `src/session/deep_buffers.*`), `Tests/Scene/USDRenderCycles_Test.cpp`
  - approach: each sample's colour = its own sum ÷ its own weight; Map() returns the flattened result.
  - verify: gtest, denoising off: flattened deep vs colour max |diff| ≤ 1e-3; flat pixels are non-empty and equal the flatten.
  - size: L
- [ ] M257.P2.T3 — Ids per sample
  - files: charlesangus/cycles (`KernelDeepSample`, `deep_write.h`, `output_driver.cpp`), `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Tests/Scene/USDRenderCycles_Test.cpp`
  - approach: store the object index per sample, converted to primId/instanceId; when samples merge, the higher-coverage id wins.
  - verify: gtest: two overlapping cards give two samples whose primId equals each card's flat primId.
  - size: M
- [ ] M257.P2.T4 — Deep opacity in the kernel (needs M286.P2.T4)
  - files: charlesangus/cycles (`deep_write.h`, `deep_buffers.h`, `output_driver.cpp`), `Engine/Nodes/Scene/CyclesAovs.cpp`, `Tests/Scene/USDRenderCycles_Test.cpp`
  - approach: an opacity-only slot (alpha, z, zback, stop, count), chosen from the channel set.
  - verify: gtest: Opacity Only gives exactly A/Z/ZBack, its flattened alpha equals Full's within 1e-4, and the slab is smaller.
  - size: M
- [ ] M257.P2.T5 — Volume segments
  - files: charlesangus/cycles (`src/kernel/integrator/shade_volume.h` :1736-1762, heterogeneous path ~1856), `Tests/Scene/USDRenderCycles_Test.cpp`, `Tests/fixtures/scene/` (small VDB or homogeneous box)
  - approach: segment samples (z = entry, zback = exit, alpha from optical depth, colour = in-scattered radiance) for homogeneous and VDB volumes (user decision 7); volumes merge only with volumes.
  - verify: gtest: a box gives zback > z; VDB fog gives segments; a DeepMerge holdout of a card in fog matches flat within tolerance.
  - size: L

## Phase 257.3: End to end

- [ ] M257.P3.T1 — Deep comp parity and cancel
  - files: `Tests/Scene/USDRenderCycles_Test.cpp`, `Engine/Nodes/Scene/docs/USDRender.md`
  - approach: USDRender (Cycles, deep) → DeepMerge with a DeepFromImage card → DeepToImage vs flat; abort during a deep render.
  - verify: gtest: the comp matches within tolerance; abort returns within 1 s and frees the slab.
  - size: M

**Verification gate:** build and ctest green on the new Cycles image; under Xvfb, a Cycles deep render of transparent cards, a sphere and fog is merged with a card and flattened, with screenshots shared with the user; peak memory reported at 1080p for Full and Opacity Only.

## M258 - MoonRay Deep Output (depends on M286 - Utility Renderer Deep Output)

## Phase 258.1: True deep in the MoonRay fork

- [ ] M258.P1.T1 — Every presence layer gets a deep record
  - files: charlesangus/moonray (`lib/rendering/pbr/integrator/PathIntegrator.{h,cc}`, `lib/rendering/pbr/handlers/ShadeBundleHandler.cc`), openmoonray pin, `tools/ci/local/Dockerfile`
  - approach: land `spike/m255/moonray/moonray-true-deep.patch`.
  - verify: `spike/m255/moonray/variants.sh` on the new image: presence vector and scalar flattened RMSE ≤ 1e-3; opaque and glass unchanged.
  - size: M
- [ ] M258.P1.T2 — Deep AOVs and volumes behind presence
  - files: charlesangus/moonray (`PathIntegrator.cc` ~1486; volume deep near `RenderContext.cc` :3465)
  - approach: behind-surface scalar samples carry deep AOVs; volume segments (colour + alpha, z..zback) accumulate across progressive passes and record behind presence.
  - verify: batch and Arras: a fog box behind a 50% card gives segments with zback > z; flattened RMSE ≤ 1e-3; batch and progressive counts agree within noise.
  - size: L

## Phase 258.2: DeepFrame over Arras

- [ ] M258.P2.T1 — DeepFrame message
  - files: a new charlesangus fork of mcrt_messages, openmoonray `.gitmodules`/pin, `tools/ci/local/build-moonray.sh`
  - approach: header (frame id, size, aperture/region, channel names, band index/count, row range) and body (uint32 counts per band row, packed float32 samples in Z order); no DCX masks.
  - verify: a fork unit test round-trips a 3-band frame bit-exactly.
  - size: M
- [ ] M258.P2.T2 — Send resolved deep in bands
  - files: a new charlesangus fork of mcrt_computation (progressive frame send)
  - approach: after the FINISHED frame, before `stopFrame`, resolve `getDeepBuffer()` with `DeepBuffer::write`'s normalisation and send 64-row bands; opacity-only sends A/Z/ZBack; stop is checked between bands; never sent for non-final passes.
  - verify: the `mr_shim.cpp` shim sees bands equal to the batch DeepBuffer; a stop mid-transfer ends within 1 s.
  - size: L
- [ ] M258.P2.T3 — hdMoonray assembles HydraDeepBuffer (needs M286.P2.T2)
  - files: charlesangus/hdMoonray (render buffer, Arras receive), vendored `HydraDeepBuffer.h`
  - approach: binding `deep` sets `output_type=deep` and `deep_z_tolerance`; bands assemble into counts, offsets and channels, served by `GetResource()` once complete.
  - verify: the probe's flattened deep vs Hydra colour RMSE ≤ 1e-4.
  - size: L

## Phase 258.3: Natron side

- [ ] M258.P3.T1 — USDRender deep on MoonRay (needs M286.P2.T3, P2.T4)
  - files: `Engine/Nodes/Scene/MoonRayAovs.{h,cpp}`, `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Tests/Scene/USDRenderMoonRay_Test.cpp`
  - approach: deep request with `deep_z_tolerance` = Merge Distance; Opacity Only gives the opacity-only channel set; host `DeepCompaction`.
  - verify: gtest: deep has R G B A Z ZBack; Opacity Only gives exactly A/Z/ZBack with alpha equal to Full's within 1e-4; a DeepMerge holdout matches flat; abort during transfer returns within 1 s.
  - size: M

**Verification gate:** build and ctest green on the new MoonRay image; under Xvfb, a MoonRay deep render of presence cards and fog is merged with a card and flattened, with screenshots shared with the user; band count and bytes per frame logged at 1080p.

Phases 258.1 and 258.2 could each be their own milestone; 258.3 needs both.
