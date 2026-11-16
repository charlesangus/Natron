# M255.P1.T2 — RenderSettings-driven AOVs on Cycles

**Cycles does not consume RenderSettings, RenderProduct or RenderVar prims, and `UsdImagingGLEngine` does not turn them into AOVs for it.** The engine only records which settings prim is active.

Probe: `rendersettings/rs_probe.cpp` via `run.sh`, in `ghcr.io/charlesangus/natron-dev:6c97a956d295d586` (gcc-toolset-14, `-std=gnu++20`). Output is in `out/probe.log` and `out/stage.usda`.
- `/Render/settings` names camera `/World/cam`, resolution 96x48 and `cycles:samples`. It is also set in the stage metadata `renderSettingsPrimPath`.
- `/Render/product` has 7 RenderVars: raw `color`, `depth` and `normal`; primvar `probeScalar`; lpe `C<RD>L`; raw `cycles:diffuse_direct`; bare `diffuse_direct`.
- Each var carries `driver:parameters:aov:name` and `natron:layer`.

| Run | What it does | What came back |
|---|---|---|
| A | Prims only, no `SetRendererAovs`, no camera | Active path is `/Render/settings`. Buffers are 64x64, not 96x48. Color is 0 and depth is 1e10, so the settings camera was not used. Only the engine defaults (color, depth, primId) come back; every RenderVar name is null. |
| B | A plus `SetActiveRenderSettingsPrimPath` | Same as A |
| C | Reference: `SetCameraPath`, color | Lit image; depth min 3.6 |
| D | Natron reads the prims itself (`UsdRenderComputeSpec`) and calls `SetRendererAovs` | 96x48 from the spec's camera and resolution. color, depth and normal have data. `primvars:probeScalar`, `lpe:C<RD>L`, `cycles:diffuse_direct` and `diffuse_direct` are null. Both per-var attributes survive in `RenderVar.namespacedSettings`. |

## Citations
hdCycles (fork 90d7cb7, `src/hydra`):
- Supported Bprims are only `renderBuffer` and `openvdbAsset` (render_delegate.cpp:65-70, 341-352).
- AOVs are keyed by `aovName` through a fixed map (session.cpp:21-27, 149-178; output_driver.cpp:48, 54).
- `aovSettings`, `HdRenderSettings` and `renderSettings` have no hits.

OpenUSD v26.08:
- engine.h:519 calls scene-defined render settings "Support is WIP".
- `SetActiveRenderSettingsPrimPath` only informs the scene-globals index (engine.cpp:2046-2060). Reading the stage metadata is the legacy path (engine.cpp:578-606).
- `SetRendererAovs` passes names only (engine.cpp:1846-1861).
- Per-AOV `aovSettings` reach the delegate only through `HdAovDescriptor` (taskControllerSceneIndex.cpp:1564, 1727), and that index is a protected engine member (engine.h:825).
- A renderSettings prim is passed on only if the delegate supports that Bprim (sceneIndexAdapterSceneDelegate.cpp:272).

How other tools use the prims:
- usdrecord takes the camera and requests color only, expecting the delegate to write the products (usdrecord:305-378; frameRecorder.cpp:339-374, 421, 486-489).
- usdview only marks a prim active (stageView.py:2457-2469).
- hdPrman is the only in-tree consumer, and only behind an env var or when there are no AOV bindings (hdPrman/renderSettings.cpp:49-58, 385-400).
- husk (Karma) maps the vars itself and makes the delegate allocate the buffers (inside_husk.md:17, 38-42; rendervar.md:156-254).

So the established way is host-side mapping. Natron already does it for camera, resolution and settings (RenderSettingsPrim.cpp:208-237; USDRender.cpp:348-365, 709).

## Natron-side mapping (USDRender, about 250 lines plus tests)
- Call `UsdRenderComputeSpec(activePrim, {renderer namespace, "driver", "natron"})` and take the union of all products' vars, deduplicated by path. Several products with different cameras: error, or add a Product knob.
- raw `sourceName` is used verbatim (so `cycles:<pass>` fits); primvar becomes `primvars:<name>`; lpe becomes `lpe:<expr>`; intrinsic is rejected.
- `dataType` sets the Natron channel layout; warn if it disagrees with the delegate's format.
- Rejected vars are detected by a null buffer after `SetRendererAovs` and reported by prim path.
- color, depth, primId and instanceId are always requested. They replace the fixed lists at HydraRenderImpl.cpp:1397-1413 and 3038, and USDRender.cpp:67-84 and 496-508 become spec-driven.
- Passing `driver:parameters:aov:*` to the delegate needs a `UsdImagingGLEngine` subclass that calls `SetRenderOutputSettings`. Cycles ignores it today.

## Authoring (USDRenderSettings, about 150 lines plus table UI)
- Write a RenderProduct child, `orderedVars`, and RenderVar children (dataType, sourceName, sourceType, metadata), alongside today's settings prim (USDRenderSettings.cpp:342-419).
- UI: a per-renderer checklist from the delegate's accepted names, plus a table for custom primvar and LPE vars.
- USDRender's Settings Prim knob already picks the prim (USDRender.cpp:125-134, 226-231, 348). Calling `SetActiveRenderSettingsPrimPath` is harmless.

## Cryptomatte and other per-AOV metadata (M285 - Utility Renderer AOVs)
- RenderVars carry it as namespaced attributes (run D), the way Karma configures Cryptomatte (cryptomatte.md, rendervar.md:162).
- The manifest itself is produced by the delegate at render time and comes back through a render stat or side buffer.

## Recommendation: both
Store AOVs as RenderVars, which is standard, round-trips through USDRead/USDWrite and carries metadata. Use the delegate-discovered checklist as the UI that authors and validates them. The mapping is host-side either way.

## Open questions
1. With several products, should USDRender union their vars or add a Product choice knob?
2. Should custom primvar and LPE vars wait for a table knob, or ship with a stopgap text field?
