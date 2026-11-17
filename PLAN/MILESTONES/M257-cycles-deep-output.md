# M257 - Cycles Deep Output

Render true per-sample deep data from Cycles into a DeepImage, in process. The delegate fills the host-side deep render-buffer handle M286 - Utility Renderer Deep Output defines (`HydraDeepBuffer`, returned from `HdRenderBuffer::GetResource()` and converted into `DeepImage` by USDRender); no other transport is designed here.

M255 - Delegate Output Spikes (P2.T1, `spike/m255/REPORT-P2T1-cycles-deep.md` at 7b386b9f7; drafts in `REPORT-P2T3-drafts.md` at a1e0d29fd, branch `spike/m255-delegate-output`) showed it is feasible: the pinned fork already carries Niik-l's deep kernel, and a +194/−6 patch (`spike/m255/deep/cycles-deep.patch`) hands a `HydraDeepBuffer` out when `cycles:deep` is bound (256x256 / 16 spp: 1.27x flat time, 1.3 samples/px mean, 16 max, 43.5 B/px handed out). This milestone lands that patch and fixes the five problems the spike found: 2 KB/px kernel memory, an absolute merge threshold that over-splits curved surfaces, a flatten that differs from beauty by up to 0.23, empty flattened AOV pixels, and no ids.

## Scope

**In:**
- The deep hand-off landed in the Cycles fork against M286's header, with a layout-version check.
- USDRender deep on Cycles, with Merge Distance driving Cycles' merge threshold and M286's host `DeepCompaction` applied.
- Fork fixes: bounded kernel memory ("Deep Working Samples", default 16), per-sample flatten correctness and filled flat pixels, primId/instanceId per sample, opacity-only slots, and volume segments carrying colour and alpha.
- End-to-end deep comp parity and cancel.

**Out:**
- Denoised deep: deep is never denoised (user, 2026-11-16); the docs say a flattened deep won't match a denoised beauty.
- Cycles Cryptomatte: after M285 - Utility Renderer AOVs.
- The `HydraDeepBuffer` header, USDRender's deep output path and `DeepCompaction`: M286 - Utility Renderer Deep Output.
- MoonRay deep: M460 - MoonRay True Deep Patch, M461 - MoonRay DeepFrame Over Arras, M258 - MoonRay Deep Output.

**Edge cases:**
- A pixel with more surfaces than Deep Working Samples: the new sample merges into its nearest slot; flatten stays within 1e-3.
- Kernel deep allocation fails: a node error, never a crash.
- `HydraDeepBuffer` layout version mismatch between the fork and Natron: a node error naming both versions.
- Deep Content = Opacity Only: alpha/Z/ZBack-only samples, and the kernel stores no colour or AOV channels.
- Volumes: segment samples (z = entry, zback = exit) with colour and alpha, for homogeneous and VDB volumes; volume samples merge only with volume samples.
- Abort during a deep render: returns within 1 s and frees the slab.

**Assumptions:**
- Unconfirmed: 16 working samples per pixel (about 1 GB at 1080p, 4 GB at 4K) is a fine default; the user raises it for dense hair or foliage. The gate reports peak memory with no budget line.

## Phase 257.1: Transport

- [ ] M257.P1.T1 — Land the deep hand-off against M286's header
  - files: charlesangus/cycles (`src/integrator/path_trace.cpp`, `src/hydra/output_driver.cpp`, `src/hydra/session.{h,cpp}`, `src/hydra/render_delegate.cpp`, vendored `HydraDeepBuffer.h`), `Engine/Nodes/Scene/HydraDeepBuffer.h` (layout version), `tools/ci/local/Dockerfile`, `tools/ci/local/build-cycles.sh`
  - approach: needs M286.P2.T2's header. Port `spike/m255/deep/cycles-deep.patch` with the header vendored byte-identical. Add a `kLayoutVersion` checked host-side; a mismatch is a node error. Bump the pin, publish the image and recreate natron-dev.
  - verify: the M255 P2.T1 probe on the new image returns the buffer with the matching version; the colour AOV is unchanged.
  - size: L

- [ ] M257.P1.T2 — USDRender deep on Cycles
  - files: `Engine/Nodes/Scene/CyclesAovs.{h,cpp}`, `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Tests/Scene/USDRenderCycles_Test.cpp`
  - approach: needs M286.P2.T3. Bind `cycles:deep`; set `deep_merge_threshold` from USDRender's Merge Distance; run the host `DeepCompaction` on the result (user, 2026-11-16: compaction controls apply to every delegate).
  - verify: gtest: cards and a sphere give a DeepImage with R G B A Z ZBack; Max Samples 2 caps every pixel.
  - size: M

## Phase 257.2: Production fixes in the fork

- [ ] M257.P2.T1 — Bounded kernel memory
  - files: charlesangus/cycles (`src/kernel/film/deep_write.h`, `src/session/deep_buffers.h`, `src/scene/film.cpp`), `Engine/Nodes/Scene/CyclesSettings.cpp`, `Tests/Scene/USDRenderCycles_Test.cpp`
  - approach: `deep_max_samples` default 16, exposed on the Cycles settings as `deepWorkingSamples` ("Deep Working Samples"). A full pixel merges into the nearest slot (`deep_write.h` near :136-150 already does). An allocation failure is a node error.
  - verify: gtest: the deep allocation at 256x256 is ≤ 16×32 B/px; a pixel with 40 overlapping cards flattens within 1e-3.
  - size: M

- [ ] M257.P2.T2 — Flatten correctness and flat pixels
  - files: charlesangus/cycles (`src/hydra/output_driver.cpp`, `src/session/deep_buffers.*`), `Tests/Scene/USDRenderCycles_Test.cpp`
  - approach: each sample's colour = its own sum ÷ its own accumulated weight; `Map()` returns the flattened result for the deep AOV's flat pixels.
  - verify: gtest, denoising off: flattened deep vs colour max |diff| ≤ 1e-3; flat pixels are non-empty and equal the flatten.
  - size: L

- [ ] M257.P2.T3 — Ids per sample
  - files: charlesangus/cycles (`KernelDeepSample`, `deep_write.h`, `output_driver.cpp`), `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Tests/Scene/USDRenderCycles_Test.cpp`
  - approach: store the object index per sample, converted to primId/instanceId host-side; when samples merge, the higher-coverage id wins.
  - verify: gtest: two overlapping cards give two samples whose primId equals each card's flat primId.
  - size: M

- [ ] M257.P2.T4 — Deep opacity in the kernel
  - files: charlesangus/cycles (`deep_write.h`, `deep_buffers.h`, `output_driver.cpp`), `Engine/Nodes/Scene/CyclesAovs.cpp`, `Tests/Scene/USDRenderCycles_Test.cpp`
  - approach: needs M286.P2.T4. An opacity-only slot (alpha, z, zback, stop, count), chosen from the requested channel set.
  - verify: gtest: Opacity Only gives exactly A/Z/ZBack, its flattened alpha equals Full's within 1e-4, and the slab is smaller.
  - size: M

- [ ] M257.P2.T5 — Volume segments
  - files: charlesangus/cycles (`src/kernel/integrator/shade_volume.h` near :1736-1762 and the heterogeneous path near :1856), `Tests/Scene/USDRenderCycles_Test.cpp`, `Tests/fixtures/scene/` (a small VDB or a homogeneous box)
  - approach: segment samples (z = entry, zback = exit, alpha from optical depth, colour = in-scattered radiance) for homogeneous and VDB volumes; volume samples merge only with volume samples.
  - verify: gtest: a box gives zback > z; VDB fog gives segments; a DeepMerge holdout of a card in fog matches flat within tolerance.
  - size: L

## Phase 257.3: End to end

- [ ] M257.P3.T1 — Deep comp parity, cancel and docs
  - files: `Tests/Scene/USDRenderCycles_Test.cpp`, `Engine/Nodes/Scene/docs/USDRender.md`
  - approach: USDRender (Cycles, deep) → DeepMerge with a DeepFromImage card → DeepToImage, compared with flat; abort during a deep render. Document that deep is never denoised, so a flattened deep won't match a denoised beauty.
  - verify: gtest: the comp matches within tolerance; abort returns within 1 s and frees the slab.
  - size: M

**Verification gate:** build and ctest green on the new Cycles image; under Xvfb, a Cycles deep render of transparent cards, a sphere and fog is merged with a card and flattened, with screenshots shared with the user (collected at F24 - Deep Rendering's UAT); peak memory reported at 1080p for Full and Opacity Only.

## Decisions

- 2026-11-05 — Volume segments are required in Cycles deep output (user): smoke must hold out correctly in a deep comp. M255 - Delegate Output Spikes has to show the transport can carry them; if it can't, bring that back to the user rather than dropping this.
- 2026-11-16 — **Deep opacity output (user, inbox):** Cycles honours the opacity-only channel set. See `PLAN/DECISIONS/2026-11-16-deep-opacity-output.md`.
- 2026-11-16 — **M255 P2.T1 findings folded in (inbox):** the five problems above are this milestone's to fix.
- 2026-11-16 — **Elaborated from M255 - Delegate Output Spikes' drafts (inbox):** compaction controls apply to every delegate; volume segments carry colour and alpha; deep is never denoised and that is documented (user, M255 P2.T3 decisions 6–8).
- 2026-11-16 — **Deep Working Samples defaults to 16, gate reports memory only (user, grill).**
