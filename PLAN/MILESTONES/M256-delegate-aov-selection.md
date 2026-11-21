# M256 - Delegate AOV Selection

Choose which AOVs and light-path expressions USDRender renders for Cycles and MoonRay, and map each to a Natron layer and channels. AOVs are stored as USD RenderProduct/RenderVar prims under the /Render prim, and the delegate-discovered AOV list is the checklist in USDRenderSettings that authors them (M255 - Delegate Output Spikes, P2.T3). Neither hdCycles nor `UsdImagingGLEngine` reads RenderVars, so Natron maps them to AOV requests itself, as husk does. Natron Utility is the first full client: M285 - Utility Renderer AOVs depends on P1.T5's per-AOV settings and metadata for its Cryptomatte manifests.

Sources: `spike/m255/REPORT-P1T1-aov-checklist.md`, `REPORT-P1T2-rendersettings-aovs.md`, `REPORT-P2T3-findings.md` and `REPORT-P2T3-drafts.md` on branch `spike/m255-delegate-output` (a1e0d29fd). The spike branch is reference only; nothing from it merges.

## Scope

**In:**
- RenderVars read into AOV requests through `UsdRenderComputeSpec`, with every product's vars unioned.
- An AOV-to-layer naming table, the requests reaching `HydraRenderImpl`, and USDRender's layers and hash following the spec.
- Per-AOV settings sent to the delegate and per-AOV metadata returned as layer metadata.
- The USDRenderSettings checklist (one section per available delegate) plus a Custom AOVs KnobTable for primvar and LPE vars.
- The Cycles fork AOV patch: `cycles:<pass>`, an `aovNames` stat, dropping elementId, light groups and primvar AOVs.

**Out:**
- Cycles Cryptomatte: later, after M285 - Utility Renderer AOVs.
- Natron Utility's own AOV list and Cryptomatte manifests: M285 - Utility Renderer AOVs.
- Deep output: M257 - Cycles Deep Output, M258 - MoonRay Deep Output, M286 - Utility Renderer Deep Output.
- A Product knob for choosing one RenderProduct: products are unioned (user, 2026-11-16).

**Edge cases:**
- Several RenderProducts: their vars are unioned and deduplicated by var path. Products that differ in camera or resolution are a node error.
- A var the active delegate rejects (a null buffer after `SetRendererAovs`), such as a typo'd primvar: a persistent node warning names each rejected var path, and the other layers render. The rejected layer is absent.
- Vars namespaced for another delegate (`moonray:*` while rendering with Cycles, `cycles:*` with MoonRay) are not passed to the active delegate at all, and raise no warning. Un-namespaced vars (`color`, `depth`, `primvars:*`, `lpe:*`) go to every delegate.
- `intrinsic` sourceType: rejected with a warning.
- elementId: dropped from the Cycles fork, and treated as rejected if a var still names it.
- No RenderVars authored: the render is unchanged from today (color, depth, primId, instanceId).
- A dataType that disagrees with the delegate's HdFormat: a warning, and the format's channel count wins.

## Phase 256.1: RenderVars to AOVs

- [x] M256.P1.T1 — Read RenderVars into AOV requests
  - files: `Engine/Nodes/Scene/RenderSettingsPrim.{h,cpp}`, `Tests/Scene/RenderSettingsPrim_Test.cpp`
  - approach: `aovRequests(prim, renderer, time)` calls `UsdRenderComputeSpec(activePrim, {renderer namespace, "driver", "natron"})` and returns the union of all products' vars, deduplicated by var path. Products that disagree on camera or resolution return an error. A raw sourceName is kept verbatim, `primvar` becomes `primvars:<name>`, `lpe` becomes `lpe:<expr>`, and `intrinsic` is rejected. Vars whose sourceName is namespaced for a delegate other than `renderer` are filtered out here. Each request carries dataType, `natron:layer` and the namespaced attributes.
  - verify: gtest on in-memory .usda: two overlapping products give a deduplicated union; each sourceType maps as stated; intrinsic is rejected; two cameras give an error; namespaced attributes survive; with renderer = Cycles a `moonray:*` var is absent from the result and a `primvars:st` var is present.
  - size: M

- [x] M256.P1.T2 — AOV-to-layer naming table
  - files: `Engine/Nodes/Scene/AovLayers.{h,cpp}` (new), `Engine/Nodes/Scene/USDRender.cpp` (layer naming near :67-84), `Engine/CMakeLists.txt`, `Tests/Scene/AovLayers_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: layer = the AOV name without its namespace, or `natron:layer` when authored. Channels: colour R,G,B(,A); normal, position and motion X,Y,Z(,W); uv U,V,W; scalars one channel. The kind comes from dataType, else a per-delegate table, else the HdFormat channel count. A dataType that disagrees with the format warns, and the format wins.
  - verify: gtest: `cycles:diffuse_direct` gives diffuse_direct.RGB, `primvars:st` gives st.UVW, depth gives depth.Z, `natron:layer` overrides; existing layer names are unchanged.
  - size: M

- [x] M256.P1.T3 — HydraRenderImpl requests the spec's AOVs
  - files: `Engine/Nodes/Scene/HydraRenderImpl.{h,cpp}` (AOV setup near :1397-1413 and :3038), `Tests/Scene/USDRender_Test.cpp`
  - approach: request color, depth, primId and instanceId plus P1.T1's requests. A null buffer after `SetRendererAovs` means rejected: collect the var paths and report them as one persistent node warning, and keep rendering the rest. elementId is treated as rejected. Any HdFormat is read into its layer, and only requested AOVs are bound.
  - verify: gtest on Cycles: RenderVars depth, normal and `primvars:nope` give the depth and normal layers, a warning naming `/Render/product/nope`, and a rendered colour layer; with no vars the render is unchanged.
  - size: L

- [x] M256.P1.T4 — USDRender layers and hash follow the spec
  - files: `Engine/Nodes/Scene/USDRender.{h,cpp}` (produced layers near :496-508, the render loop, `appendNodeContentHash`), `Tests/Scene/USDRender_Test.cpp`
  - approach: the produced layers are the active prim's spec layers for the selected renderer, cached per time. Edits to the RenderProduct and RenderVar children change the hash through M253.P2.T8's side table.
  - verify: gtest: adding a RenderVar adds the layer and invalidates; an unrelated edit does not; the channel selector lists the new layer; switching Renderer from Cycles to MoonRay drops the `cycles:*` layers.
  - size: M

- [x] M256.P1.T5 — Per-AOV settings in, delegate metadata out
  - files: `Engine/Nodes/Scene/HydraRenderImpl.{h,cpp}` (a `UsdImagingGLEngine` subclass), `Engine/Nodes/Scene/USDRender.cpp`, `Tests/Scene/USDRenderMoonRay_Test.cpp`, `Tests/Scene/USDRender_Test.cpp`
  - approach: a `UsdImagingGLEngine` subclass calls `SetRenderOutputSettings` with each var's `driver:parameters:aov:*` as `aovSettings`. `aovMetadata:<aov>` render stats become layer metadata, which M285 - Utility Renderer AOVs uses for the Cryptomatte manifest.
  - verify: gtest: a MoonRay var's `driver:parameters:aov:moonray:*` reaches hdMoonray; a test stat dictionary appears as layer metadata.
  - size: L

## Phase 256.2: Checklist authoring

- [x] M256.P2.T1 — Delegate AOV catalogue
  - files: `Engine/Nodes/Scene/DelegateSupport.{h,cpp}`, `Engine/Nodes/Scene/CyclesAovs.cpp`, `Engine/Nodes/Scene/MoonRayAovs.cpp`, `Tests/Scene/USDRenderCycles_Test.cpp`, `Tests/Scene/MoonRayAovs_Test.cpp`
  - approach: `availableAovs(renderer)` returns the delegate's `aovNames` stat when it publishes one, else a Natron-side table per delegate.
  - verify: gtest: Cycles lists its current 5 AOVs (every `cycles:<pass>` once P3.T1 lands); MoonRay lists its table; Storm lists its own.
  - size: M

- [x] M256.P2.T2 — USDRenderSettings writes RenderProduct and RenderVars
  - files: `Engine/Nodes/Scene/USDRenderSettings.{h,cpp}` (authoring near :342-419), `Engine/Nodes/Scene/docs/USDRenderSettings.md`, `Tests/Scene/USDRenderSettings_Test.cpp`
  - approach: an AOV checklist with one collapsible group per available delegate (Cycles, MoonRay, Utility, Storm), filled from P2.T1; ticks in every group are authored. Add a `customAovs` KnobTable labelled "Custom AOVs" (Name, Source Type, Data Type, Layer) for primvar and LPE vars. It writes a `product` RenderProduct, its `orderedVars` and one RenderVar per pick; unticking removes the var. Labels follow script names.
  - verify: gtest: ticking Cycles normal plus a `primvars:st` row gives two correct RenderVars; a MoonRay tick in the same node adds a `moonray:*` var that a Cycles render skips; USDWrite round-trips them; USDRender renders both Cycles layers.
  - size: L

- [x] M256.P2.T3 — Hand-written RenderVars from a file
  - files: `Tests/Scene/USDRenderSettings_Test.cpp`, `Tests/fixtures/scene/rendervars.usda` (new, small)
  - approach: USDRead loads a product with three vars and a renamed `natron:layer`; USDRender renders it with no USDRenderSettings node.
  - verify: the gtest sees the three layers under their authored names.
  - size: M

## Phase 256.3: Cycles fork AOVs

- [x] M256.P3.T1 — `cycles:<pass>`, `aovNames`, drop elementId
  - files: charlesangus/cycles `natron/deep-5.2` (`src/hydra/render_delegate.cpp` near :385 and :416-439; `src/hydra/session.cpp` near :21-28 and :149-178), `tools/ci/local/Dockerfile`
  - approach: apply patch steps 1, 2 and 6 from `spike/m255/REPORT-P1T1-aov-checklist.md`; a pass's format follows its channel count. Bump the pin, publish the image and recreate natron-dev.
  - verify: the P1.T1 probe gets non-null `cycles:diffuse_direct`, `cycles:ao` and `cycles:mist`; the stat lists the passes; no elementId warning appears.
  - size: M

- [x] M256.P3.T2 — Cycles pass kinds in Natron
  - files: `Engine/Nodes/Scene/CyclesAovs.{h,cpp}`, `Engine/Nodes/Scene/AovLayers.cpp`, `Tests/Scene/USDRenderCycles_Test.cpp`
  - approach: a table of Cycles pass names to kind, used by P1.T2.
  - verify: gtest: diffuse_direct gives RGB, uv gives UVW, mist gives one channel; the light passes sum to beauty with denoising off.
  - size: M

- [x] M256.P3.T3 — Light groups and primvar AOVs in the Cycles fork
  - files: charlesangus/cycles (`src/hydra/light.cpp` near :370-398, `material.cpp` near :578-589, :742 and :766, `session.cpp` near :74-85), `tools/ci/local/Dockerfile`, `Tests/Scene/USDRenderCycles_Test.cpp`
  - approach: `cycles:lightgroup:<name>` from an attribute on lights, meshes and the background; `primvars:<name>` built like the instanceId AOV; materials re-sync when the requested set changes.
  - verify: gtest: two light groups sum to the direct lighting; `primvars:st` matches an authored ramp.
  - size: L

**Verification gate:** build and ctest green on the new Cycles image; under Xvfb, three Cycles passes ticked plus one custom primvar var give four layers in the viewer and in one EXR, and a MoonRay tick in the same node adds no warning to the Cycles render; screenshots shared with the user. Stack M257 - Cycles Deep Output's fork commits on P3's.

## Decisions

- 2026-11-03 — AOVs are authored as RenderProduct/RenderVar under the /Render prim through USDRenderSettings (M253.P2), read with M253.P2.T3's library (UsdRenderComputeSpec), and hashed through M253.P2.T8's side table (user decision on M253, 2026-11-03).
- 2026-11-16 — **Elaborated from M255 - Delegate Output Spikes' drafts (inbox):** RenderVars are stored and the delegate checklist authors them; several products are unioned (different cameras or resolutions are an error); custom primvar and LPE vars use a KnobTable; the Cycles fork AOV patch (`cycles:<pass>`, `aovNames`, light groups, primvar AOVs) is in scope; elementId is dropped from the Cycles fork (user, M255 P2.T3 decisions 1–4).
- 2026-11-16 — **Rejected vars warn and the rest render (user, grill):** a switch of renderer must never break a comp.
- 2026-11-16 — **Checklist has one section per delegate (user, grill):** USDRender or the viewer may render with a renderer other than Default Renderer, so picks for every delegate are authored.
- 2026-11-16 — **Only the selected renderer's vars are passed (user, grill):** vars namespaced for another delegate are filtered out before `SetRendererAovs`, with no warning; un-namespaced vars go to every delegate.
- 2026-11-20 — Phase 256.1 in d64b333a7. hdMoonray reads `parameters:moonray:<attr>`, so driver keys are also sent as `parameters:<x>`. Duplicate AOV names and layer-name collisions are rejected with a warning. **PM calls:** an unrecognised one-channel AOV is named `Y` (depth names get Z, id names get id). A product camera that differs from the settings prim's camera is honoured, matching the reference DCC, in Phase 256.2. Build running.
- 2026-11-20 — Phase 256.1 is gate-green on 432fc93b5. Its test fixes: st is two channels as authored (texCoord2f), and the other-delegate setting nudge uses a stand-in renderer, since Storm declares no settings in the test process.
- 2026-11-20 — Phase 256.2:
  - `availableAovs` comes from each delegate's aovNames stat, with Natron tables as the fallback. The Storm table was checked against v26.08 hdSt: color, depth, primId, instanceId, elementId, edgeId, pointId and Neye; no normal or Peye.
  - The new KnobRenderVars table knob drives the checklist.
  - Products override the settings prim's camera, resolution, pixel aspect, conform policy, data window, motion blur and depth of field.
  - The scene-rendervars.usda fixture is added.

  **PM calls, matching the reference DCC:** products live at /Render/Products/<settings prim name>, and vars share /Render/Products/Vars; a shared var's unset data type and layer are blocked so input values can't leak in. Gate running.
- 2026-11-21 — Phase 256.2 gate green on 481fea51e. Phase 256.3 dispatched.
- 2026-11-21 — Phase 256.3:
  - **Fork:** charlesangus/cycles branch natron/m256-aovs at 172450ba4, off natron/deep-5.2. It adds 22 `cycles:<pass>` buffers, `cycles:lightgroup:<name>` (lights read `inputs:cycles:lightgroup`; geometry reads the `primvars:cycles:lightgroup` primvar) and `primvars:<name>` AOVs, publishes an `aovNames` stat, and drops elementId. The Dockerfile pin is bumped; M257's fork commits go on top of it.
  - **Natron:** `CyclesAovs::passLayout` sets each pass's kind.
  - **PM calls:** a 2-channel UV var takes Cycles' 3-channel buffer quietly; derived layer names map ':' to '_' (lightgroup_key); the light-group attribute names follow the reference tools' pattern.
  - **Noted:** Cycles light groups are direct plus indirect.
  - **Next:** the gate (with the new dev image), then the Xvfb four-layer check and screenshots.
- 2026-11-21 — The Phase 256.3 gate (run 44864083630) failed 4 tests. Three are Cycles fork bugs; the tests are right:
  1. Lights look up `cycles:lightgroup`, but USD 26.08's light data source needs `inputs:cycles:lightgroup`.
  2. Primvar AOVs return a 4-channel buffer.
  3. UpdateScene overwrites an explicit `cycles:background:lightgroup` with the dome's group.

  Fork fixes are in progress, then a pin bump. The fourth was a test bug: Cycles and Storm render any `primvars:*` var (a missing primvar renders zeros), so the rejected-var fixture is now a raw var. **PM call (reference DCC behaviour):** a typo'd primvar var renders black with no warning; Natron doesn't traverse the stage to check primvars exist. The milestone's 'typo'd primvar' example now reads 'a var the renderer has no pass for'.

