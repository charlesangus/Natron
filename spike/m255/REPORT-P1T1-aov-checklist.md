# M255.P1.T1 — Delegate-discovered AOV checklist on Cycles

Probe: `spike/m255/aov/aov_probe.cpp` (driven by `run.sh`) uses `UsdImagingGLEngine` as `HydraRenderImpl.cpp` does (gpuEnabled=false, `SetRendererAovs`, render to convergence, `GetAovRenderBuffer`/Resolve/Map), plus the delegate's `GetDefaultAovDescriptor` and captured Tf warnings. It ran in `ghcr.io/charlesangus/natron-dev:6c97a956d295d586`; the running natron-dev container has no hdCycles. That image's hdCycles predates the Dockerfile pin (90d7cb7), but the AOV code is unchanged since v5.2.0 apart from locking. It builds only with gcc-toolset-14 `g++ -std=gnu++20` (clang++ hits a library mismatch). Raw log: `aov/out/probe.log`.

## What Cycles returns
Each AOV was tested alone with color, then all together.

| AOV | HdFormat | Notes |
|---|---|---|
| color | Float32Vec4 | RGBA |
| depth | Float32 | Distance from the camera; 1e10 where nothing is hit (src/kernel/film/read.h:117) |
| primId | Int32 | -1 where nothing is hit |
| instanceId | Int32 | -2 on prims that aren't instanced, -1 where nothing is hit |
| normal | Float32Vec3 | World-space shading normal (data_passes.h:77-79) |
| elementId | Int32 | Every pixel is 0 (not even its -1 clear value); `WARNING: Unknown pass elementId` (session.cpp:163-167) on every render, because USD adds elementId whenever color is requested |

`GetRendererAovs()` lists only `color primId depth normal`. It checks a fixed list from USD, so it can't be used to discover the delegate's passes.

## Rejected requests (all fail silently)
These were rejected:
- the other standard names (depthStencil, cameraDepth, edgeId, pointId, Peye, Neye, patchCoord, primitiveParam);
- `primvars:*`, `lpe:*` (four expressions tried) and `shader:*`;
- bare Cycles pass names (`diffuse_direct`, `ao`, `uv`, `mist`, `cryptomatte`) and `cycles:diffuse_direct`.

For each one:
- the descriptor format is Invalid (render_delegate.cpp:439);
- `SetRendererAovs` still returns true;
- `GetAovRenderBuffer` returns null, before the first render and after;
- nothing is logged, because USD drops the name before the delegate sees it.

The earlier source-only note ("dropped with Unknown pass") is wrong for our path: only elementId reaches the delegate. A check that needs no delegate change: after `SetRendererAovs`, a non-null buffer means the AOV was accepted. elementId is the one false positive.

## LPEs
Cycles has no light-path-expression support. The nearest equivalents:
- fixed light passes: diffuse, glossy, transmission and volume, each direct, indirect and colour, plus emission, background and ao;
- light groups (scene/pass.h:58, object.h:72, background.h:31, film.cpp:528).

hdCycles maps 5 of the 53 named Cycles passes (session.cpp:21-28; pass.cpp:45-110).

## Delegate patch sketch (Cycles fork, `src/hydra/`)
1. Accept `cycles:<pass>` names, with the format following the pass's channel count (1/3/4 → Float32/Vec3/Vec4). Edits go in render_delegate.cpp:416-439 and session.cpp:21-28 and 149-178; output_driver.cpp already reads passes by name. About 60 lines.
2. Publish the accepted names as an `aovNames` render stat (render_delegate.cpp:385), which `GetRenderStats()` passes to Natron unchanged. About 15 lines. The no-patch alternative is a Natron-side table plus the null-buffer check.
3. Light groups: `cycles:lightgroup:<name>` as a combined pass, with lights, emitting meshes and the background tagged by an attribute (light.cpp:370-398, session.cpp UpdateScene). About 50 lines.
4. `primvars:<name>` AOVs, built like the existing instanceId AOV (material.cpp:578-589, called at 742 and 766; session.cpp:74-85). Materials must re-sync when the requested set changes. About 80 lines.
5. Cryptomatte needs its own patch with a manifest Natron would read: 100+ lines on each side.
6. Drop the elementId descriptor (render_delegate.cpp:434) so the dead binding and its warning go away.

Steps 1 and 2 give a working checklist of the fixed Cycles passes for about 75 lines plus a Cycles image-layer rebuild. Steps 3 and 4 add about 130 more.

## Natron layer naming (proposal)
- The layer is the AOV name without its prefix (`cycles:diffuse_direct` → `diffuse_direct`).
- Colour passes get R,G,B(,A); normal, position and motion get X,Y,Z(,W); uv gets U,V,W; single values get one channel (`depth.Z`, `primId.id`).
- Cycles doesn't say which passes are colours and which are vectors, so Natron needs a table for that.
- New picks are appended to the fixed lists at HydraRenderImpl.cpp:1397-1413 and 3038.

## Open questions
1. Is a Cycles fork patch in scope, or does the checklist stay Natron-side (5 AOVs on Cycles today)?
2. Should the fork stop advertising elementId?
3. Recreate natron-dev from the current published image before any Cycles AOV work runs in it.
