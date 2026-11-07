# M252 - MoonRay Rendering

USDRender renders a scene through hdMoonray and produces `rgba`, `depth.Z`, `primId.id` and
`normal`, CPU only. The twin of M251 - Cycles Rendering on the MoonRay delegate from M250 -
Bundle MoonRay Delegate, through M248 - Render Delegate Selection's plumbing. Reuse the
output-layer code M251 - Cycles Rendering wrote instead of duplicating it.

## Scope

**In:**
- `Renderer = MoonRay` renders the stage from the chosen camera at the project format.
- Output layers `rgba`, `depth.Z`, `primId.id` (same metadata as Storm) and `normal`.
- Scene-linear output, abort on cancel, thread count from the scheduler budget.
- MoonRay's scene-conversion limits reported as node warnings: it needs materials and lights expressed in its own schema (`MoonrayMaterial` / UsdPreviewSurface translation), so unsupported prims are named, not silently dropped.

**Out:**
- Other AOVs and LPEs — M256 - Delegate AOV Selection.
- Settings knobs — M253 - Delegate Render Settings.
- Progressive viewer — M254 - Progressive Viewer Render.
- Deep — M258 - MoonRay Deep Output.
- GPU/XPU, Arras distribution — none.

**Edge cases:**
- hdMoonray converts only some USD light/material types: a scene using others renders with warnings naming them.
- MoonRay prints heavy log output: route it to Natron's log at debug level, errors as node errors.
- Stochastic output: fixed seed and samples with a tolerance, as M251 - Cycles Rendering.

**Assumptions:**
- Unconfirmed: hdMoonray exposes `color`, `depth`, `primId`, `normal`. Read the delegate source and mark recollection; if an AOV is absent derive or drop with a decision.
- Unconfirmed: MoonRay's output is scene-linear; confirm against the delegate.

## Phase 252.1: Render path

- [x] M252.P1.T1 — Beauty and depth from MoonRay
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/USDRender.cpp`, `Engine/Nodes/Scene/MoonRayAovs.cpp`
  - approach: share M251 - Cycles Rendering's delegate-agnostic read-back; add only MoonRay's AOV token names and its converged-state check. Route MoonRay logging to Natron's log.
  - verify: a unit test renders a unit cube headless and gets non-black `rgba` and a `depth.Z` equal to the camera distance at the centre pixel (within tolerance).
  - size: L

- [x] M252.P1.T2 — primId, normal and unsupported-prim warnings
  - files: `Engine/Nodes/Scene/MoonRayAovs.cpp`, `Engine/LayerRegistry.cpp`, `Engine/Nodes/Scene/USDRender.cpp`
  - approach: same as M251.P1.T2 for `primId.id` and `normal`; collect the delegate's unsupported prim/material notices into one persistent warning per kind.
  - verify: a two-cube scene gives two distinct `primId.id` values naming the right prims; a scene with a deliberately unsupported light type warns by name and still renders.
  - size: M

- [x] M252.P1.T3 — Cancel and thread budget
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/MoonRayAovs.cpp`
  - approach: as M251.P1.T3 against MoonRay's thread and stop calls; verify names against the delegate's settings list.
  - verify: abort returns within a second with no MoonRay threads left; concurrent renders stay within the thread budget.
  - size: M

- [x] M252.P1.T4 — Reference render test
  - files: `Tests/Scene/USDRenderMoonRay_Test.cpp`, committed reference scene and image under `Tests/`
  - approach: fixed seed and samples, tolerance-based comparison with its rationale in the test.
  - verify: passes in CI release; identical on rerun on the same host.
  - size: M

## Phase 252.2: MoonRay textures and Coding Errors (Lobby UAT, 2026-11-05)

- [x] M252.P2.T1 — Read plain jpg and png textures
  - files: tools/ci/local/build-moonray.sh (patch), a delegate test and fixture
  - approach: All 1,256 Lobby images are plain jpg or png; 331 unique files fail with `Invalid image file ...: image was untiled`, then `FATAL: failed to open texture file`, leaving the interior black and 60,659 NaN pixels. `libtexturing_sampler.so` carries `accept_untiled`, `autotile` and `automip` strings; the exact setting was not read from source. Allow untiled reads with a patch, or convert to a cache on demand. Asking users to run `maketx` is not a fix.
  - verify: a textured quad with a `.jpg` matches the same image through `maketx` (was 0.012 against 0.12 average); the Lobby logs no texture failure and has no NaN pixels.
  - size: L
- [ ] M252.P2.T2 — Separate the fully-textured Lobby's render time
  - files: none expected unless a fault is found; a note in `## Decisions`
  - approach: Open: with every image mip-tiled, a 4-sample 1080p frame did not finish in 560 s, against 41 s when textures failed. Slow, hung, or a side effect of the test copy, which stored mip-tiled TIFFs under `.jpg` and `.png` names. Measure after M252.P2.T1; file a fix if it is a fault.
  - verify: the Lobby renders a frame through MoonRay with all textures loaded, or the cause of the stall is identified and recorded.
  - size: M
- [x] M252.P2.T3 — The prim warning names the failing texture files
  - files: the hdMoonray render path that raises `MoonRay could not render these prims as authored`, a test
  - approach: The warning names 115 meshes when the fault is their textures. Name the texture files when a texture is the cause.
  - verify: a fixture with an unreadable texture warns with that file's name.
  - size: M
- [x] M252.P2.T4 — Trace hdMoonray's Coding Errors per textured material
  - files: tools/ci/local/build-moonray.sh (patch), a test
  - approach: Two `Attempted to get value of type 'TfToken' from empty VtValue` and one `MarkAllRprimsDirty ... requires emulation` per textured material, also on a one-material scene. Not traced. If the dirty call does nothing without emulation, material edits may not re-sync geometry, so check that as well.
  - verify: a one-material scene logs no Coding Error; a material edit visibly reaches the next MoonRay render.
  - size: M

**Verification gate:** CI green including the MoonRay test; a MoonRay USDRender comp renders headless with `NatronRenderer -w`; a rendered frame with its `depth.Z`, `normal` and `primId.id` views is shared with the user; Storm and Cycles references unchanged.

## Decisions

- 2026-11-07 — F3 approved by the user at e87d296ea. M252.P2.T2 (the fully-textured Lobby's render time) stays open as a post-merge check.

- 2026-11-06 — M252.P2.T3 and T4 written (not built): source puts both Coding Errors on lights, not materials (`Light.cc:408,420,433`), so they scaled with one light in the reported scene. `MarkAllRprimsDirty` posts the error but still applies its bits (`changeTracker.cpp` ~998), so it's noise. A material edit does reach MoonRay in the normal case: `Material::Sync` re-runs `updateTerminal` on the same object, reused by name; it goes stale only if the terminal node changes class. Confirm after the image rebuild; the material-edit half has no test yet.
- 2026-11-06 — M252.P2.T1 written, box stays open until the image rebuild and Lobby check. `TextureSampler.cc:58-70` hardcodes `automip=0`, `autotile=0`, `accept_untiled=0`, `accept_unmipped=0`; patch sets the first three to 1 and keeps `autotile=0`, because JPEG scanlines read forwards and any backward seek reopens the file (`jpeginput.cpp:633`). At OIIO lookup level the patch matches `maketx` within 1/255; accept flags without automip differ by up to 0.67. Bears on M252.P2.T2: once decoded textures outgrow the 4000 MB default cache, each miss re-decodes a whole JPEG and the render stalls (331 4K files decode to about 16 GB). Next steps there: `texture_cache_size`, or convert on demand. The Lobby isn't on this host, so that check belongs to the `gpu` host or the F3 - Cycles And MoonRay Delegates UAT.

- 2026-10-27 — **Share the read-back with Cycles (planner):** one delegate-agnostic path, per-delegate files hold only AOV token names and quirks.
- 2026-10-28 — **No stand-in light for MoonRay (implementer):** Natron's headlight is a Hydra simple light set on Storm's engines only; hdMoonray renders no simple light, so a MoonRay render of a stage without lights is black, with no injected light. Revisit together with M251 - Cycles Rendering's open question on its 0.5 grey no-light background.
- 2026-11-02 — Audit: implemented at e1dc30b28 and ff10ce7a5 (MoonRayAovs, HydraRender read-back, log capture and session failure, one warning per unsupported light kind, abort and thread-budget tests). Deviation in T4: no committed reference image; USDRenderMoonRay_Test checks depth, normals and cube placement analytically and compares a rerun exactly (RerunIsIdentical). Pushed, clean, no PR, 23 behind origin/main; stacked on M250 and M251 (audit).
- 2026-11-03 — Gate fixes: implicit UsdGeom shapes reach MoonRay as meshes (hdMoonray accepts only mesh/curves/points/volume; 8c6d3ffb2). MoonRay final renders are not bit-identical (single-machine mcrt_computation is progressive-only; overlapping passes atomically add samples), so RerunIsIdentical allows one half-float step. Bit-identical would need patching mcrt_computation for batch mode plus scalar execution, losing in-render cancel; not pursued.
