# M255.P2.T3 — Findings and decision drafts

Sources: the four reports in `spike/m255/` on `spike/m255-delegate-output`. Code paths are from current main.

## 1. Scope questions, one line each
- **AOV mechanism:** store AOVs as RenderVar prims and use the delegate's accepted AOVs as the checklist that authors them. Neither hdCycles nor `UsdImagingGLEngine` reads RenderVars, so Natron maps them itself, as husk does (P1.T2 run D; engine.h:519; engine.cpp:1846-1861).
- **Cycles deep in-process:** yes. The pinned fork has the deep kernel, and a +194/−6 patch hands out `HydraDeepBuffer` through `GetResource()` when `cycles:deep` is bound. Deep takes 1.27x the flat time and hands out 43.5 B/px (P2.T1).
- **MoonRay deep with no file and no extra process:** yes, over Arras (decision (d)). The progressive path fills DeepBuffer as batch does, and flat AOVs survive alongside it. A 1080p frame is about 31 MiB, 1.8 MiB per 64-row band. The +76/−12 true-deep patch fixes the first-surface limit (RMSE 0.106 → 0.0001) (P2.T2 and its Follow-up).
- **Does Hydra need a variable-length AOV?** No. `GetResource()` returning a `VtValue` that holds a `shared_ptr<const HydraDeepBuffer>` crosses into a separately compiled consumer (P2.T1). The cost is a header the forks keep byte-identical to Natron's, with a layout-version check.
- **Per-AOV metadata for Cryptomatte:** RenderVars carry it as namespaced attributes, which survive in `namespacedSettings`. The manifest comes back as a render stat. Sending settings in needs a `UsdImagingGLEngine` subclass that calls `SetRenderOutputSettings`.

## 2. Recommendations (one option per fork)
- **AOV mechanism:** RenderVars are stored; the delegate checklist is the authoring UI. Rejected names are detected by a null buffer after `SetRendererAovs`.
- **Cycles fork AOV patch:** `cycles:<pass>`, the `aovNames` stat and dropping elementId first (about 75 lines). Light groups and primvar AOVs next (about 130 lines). Cycles Cryptomatte later, after M285 - Utility Renderer AOVs.
- **Cycles deep fixes (M257 - Cycles Deep Output):**
  - **Memory:** lower `deep_max_samples` to 16 and expose it. A full pixel already merges into the nearest slot (`deep_write.h:136-150`), so no count-then-allocate pass is needed.
  - **Slots:** use slimmer slots for opacity-only.
  - **Merge threshold:** set it from USDRender's Merge Distance.
  - **Flatten:** resolve each sample by its own accumulated weight.
  - **Flat pixels:** fill the deep buffer's flat pixels with the flattened result.
  - **Ids:** store each sample's object index, converted to primId/instanceId.
- **MoonRay:** banded DeepFrame over Arras, plus the true-deep patch in our fork.
- **MoonRay's DCX writer:** don't fix it. It is batch-only and outside our path.
- **Deep Z tolerance:** Natron sets MoonRay's `deep_z_tolerance` from Merge Distance, the same knob that drives Cycles' threshold.
- **Compaction:** USDRender runs M286's `DeepCompaction` on every delegate's buffer.

## 3. User decisions needed (recommended answer in brackets)
1. With several RenderProducts, union their vars or add a Product knob? [Union, deduplicated by var path. Different cameras or resolutions are an error.]
2. Custom primvar and LPE vars: table knob or stopgap? [Table knob now. `KnobTable` exists (`Engine/KnobTypes.h:1600`).]
3. Is the Cycles fork AOV patch in scope for M256 - Delegate AOV Selection? [Yes, as M256 phase 256.3.]
4. Drop elementId from the Cycles fork? [Yes.]
5. Fix MoonRay's DCX writer? [No; record it as a known upstream bug.]
6. Do USDRender's compaction controls apply to every delegate? [Yes.]
7. Must volume deep segments carry colour as well as alpha? Cycles' deep writes alpha-only samples for homogeneous volumes and nothing for VDB volumes (`shade_volume.h:1736-1762`). MoonRay's volume deep is untested. [Colour plus alpha on both, each in its own phase, about L per delegate.]
8. Deep is never denoised, so flattened deep can't match a denoised beauty. [Accept it, document it, and measure with denoising off.]

## 4. Draft task lists
The full drafts are long. They are summarised here and kept verbatim in `REPORT-P2T3-drafts.md` for `/cat-plan` to paste.

- **M256 - Delegate AOV Selection:**
  - 256.1 RenderVars to AOVs: spec reading, the AOV-to-layer table, HydraRenderImpl requests, USDRender layers and hash, per-AOV settings and metadata.
  - 256.2 Checklist authoring: delegate catalogue, USDRenderSettings writing RenderProduct and RenderVars with a Custom AOVs table, file-authored vars.
  - 256.3 Cycles fork AOVs: `cycles:<pass>`/aovNames/elementId, pass kinds, light groups and primvar AOVs.
- **M257 - Cycles Deep Output (depends on M286):**
  - 257.1 Transport: land the patch against M286's header; USDRender deep on Cycles.
  - 257.2 Fork fixes: bounded memory, flatten correctness and flat pixels, ids, deep opacity, volume segments.
  - 257.3 End-to-end comp parity and cancel.
- **M258 - MoonRay Deep Output (depends on M286):**
  - 258.1 True deep in the fork: the presence patch, plus deep AOVs and volumes behind presence.
  - 258.2 DeepFrame over Arras: message, banded send, hdMoonray assembly.
  - 258.3 Natron side.
  - Phases 258.1 and 258.2 could be split into separate milestones.

## Risks
- `HydraDeepBuffer` crosses a plugin boundary: the delegates are built into the image before Natron. Vendored headers plus a layout-version check guard it.
- Tearing down the MoonRay Hydra delegate after an Arras render crashes.
- MoonRay test scenes must use presence (`DwaBaseMaterial`), not `UsdPreviewSurface` opacity.
- Recreate natron-dev from the published image before any Cycles AOV or deep work.
- Niik-l's per-pixel merge under heavy contention is unmeasured.
