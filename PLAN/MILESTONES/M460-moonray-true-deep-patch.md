# M460 - MoonRay True Deep Patch

Make MoonRay's deep output true deep in our MoonRay fork. Upstream MoonRay records only the first surface each camera ray hits for presence ("first hit only for now", `ShadeBundleHandler.cc:211`), so flattened deep differs from beauty wherever surfaces overlap (RMSE 0.106). M255 - Delegate Output Spikes' +76/−12 patch (`spike/m255/moonray/moonray-true-deep.patch`, c8e1892ee, branch `spike/m255-delegate-output`) records every presence layer and brings the flatten to RMSE 0.0001 in vector, scalar and Hydra-over-Arras renders. This milestone lands it, adds deep AOVs and volume segments behind presence, and lowers the deep Z tolerance default. Fork-only: no Natron engine code changes.

## Scope

**In:**
- The true-deep presence patch in charlesangus/moonray, with the openmoonray pin bumped and the image republished.
- Deep AOVs on behind-surface samples, and volume segments (colour + alpha, z..zback) accumulated across progressive passes and recorded behind presence.

**Out:**
- The DeepFrame Arras message and hdMoonray assembly: M461 - MoonRay DeepFrame Over Arras.
- USDRender wiring: M258 - MoonRay Deep Output.
- MoonRay's DCX writer: not fixed (user, 2026-11-16).

**Edge cases:**
- Opaque and glass scenes: deep output unchanged from upstream.
- Batch and progressive (Arras) renders: sample counts agree within noise.

## Phase 460.1: True deep in the MoonRay fork

- [ ] M460.P1.T1 — Every presence layer gets a deep record
  - files: charlesangus/moonray (`lib/rendering/pbr/integrator/PathIntegrator.{h,cc}`, `lib/rendering/pbr/handlers/ShadeBundleHandler.cc`), the openmoonray pin, `tools/ci/local/Dockerfile`, `tools/ci/local/build-moonray.sh`
  - approach: land `spike/m255/moonray/moonray-true-deep.patch` on the fork's Natron branch; bump the pin; publish the image and recreate natron-dev.
  - verify: `spike/m255/moonray/variants.sh` on the new image: presence vector and scalar flattened RMSE ≤ 1e-3; opaque and glass outputs unchanged.
  - size: M

- [ ] M460.P1.T2 — Deep AOVs and volumes behind presence
  - files: charlesangus/moonray (`lib/rendering/pbr/integrator/PathIntegrator.cc` near :1486; volume deep near `lib/rendering/rndr/RenderContext.cc` :3465)
  - approach: behind-surface scalar samples carry deep AOVs; volume segments (colour and alpha, z..zback) accumulate across progressive passes and record behind presence.
  - verify: batch and Arras renders of a fog box behind a 50% presence card give segments with zback > z, flattened RMSE ≤ 1e-3, and batch and progressive counts that agree within noise.
  - size: L

**Verification gate:** `variants.sh` and the fog probe pass on the published image; existing MoonRay ctest (`ctest -R MoonRay`) green on that image.

## Decisions

- 2026-11-16 — **Split from M258 - MoonRay Deep Output (user, inbox and grill):** fork-only work with no dependency on M286 - Utility Renderer Deep Output, so it can run now.
- 2026-11-16 — **Volume deep segments carry colour and alpha (user, M255 P2.T3 decision 7).**
- 2026-11-22 — T1 landed on the forks (moonray 799d47e4, openmoonray c4edab6) and pinned in a4bb52fe3; the remote fast gate with the rebuilt image is green (full ctest). Still to run: `variants.sh` in the image (presence flattened RMSE ≤ 1e-3; opaque and glass unchanged) before checking T1 off.
