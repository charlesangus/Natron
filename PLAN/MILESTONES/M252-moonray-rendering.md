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

- [ ] M252.P1.T1 — Beauty and depth from MoonRay
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/USDRender.cpp`, `Engine/Nodes/Scene/MoonRayAovs.cpp`
  - approach: share M251 - Cycles Rendering's delegate-agnostic read-back; add only MoonRay's AOV token names and its converged-state check. Route MoonRay logging to Natron's log.
  - verify: a unit test renders a unit cube headless and gets non-black `rgba` and a `depth.Z` equal to the camera distance at the centre pixel (within tolerance).
  - size: L

- [ ] M252.P1.T2 — primId, normal and unsupported-prim warnings
  - files: `Engine/Nodes/Scene/MoonRayAovs.cpp`, `Engine/LayerRegistry.cpp`, `Engine/Nodes/Scene/USDRender.cpp`
  - approach: same as M251.P1.T2 for `primId.id` and `normal`; collect the delegate's unsupported prim/material notices into one persistent warning per kind.
  - verify: a two-cube scene gives two distinct `primId.id` values naming the right prims; a scene with a deliberately unsupported light type warns by name and still renders.
  - size: M

- [ ] M252.P1.T3 — Cancel and thread budget
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/MoonRayAovs.cpp`
  - approach: as M251.P1.T3 against MoonRay's thread and stop calls; verify names against the delegate's settings list.
  - verify: abort returns within a second with no MoonRay threads left; concurrent renders stay within the thread budget.
  - size: M

- [ ] M252.P1.T4 — Reference render test
  - files: `Tests/Scene/USDRenderMoonRay_Test.cpp`, committed reference scene and image under `Tests/`
  - approach: fixed seed and samples, tolerance-based comparison with its rationale in the test.
  - verify: passes in CI release; identical on rerun on the same host.
  - size: M

**Verification gate:** CI green including the MoonRay test; a MoonRay USDRender comp renders headless with `NatronRenderer -w`; a rendered frame with its `depth.Z`, `normal` and `primId.id` views is shared with the user; Storm and Cycles references unchanged.

## Decisions

- 2026-10-27 — **Share the read-back with Cycles (planner):** one delegate-agnostic path, per-delegate files hold only AOV token names and quirks.
