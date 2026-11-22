# M513 - MoonRay OIDN Denoiser

MoonRay's denoiser does not work in our build. The knob tooltips say so (`Engine/Nodes/Scene/MoonRaySettings.cpp:122`, `:158-171`): Enable Denoise asks for the OptiX denoiser, which this CPU-only build lacks. `enableOIDN` only turns denoising on again, and no render output is marked as the albedo or normal input. MoonRay is built from Natron's fork (`github.com/charlesangus/openmoonray`, branch `natron/v2026.29.1`, pinned by `NATRON_MOONRAY_COMMIT` at `tools/ci/local/Dockerfile:70`) by `tools/ci/local/build-moonray.sh`. Natron's changes are commits on the forks of moonray, hdMoonray and the other submodules, and the build applies no patches. Make OIDN the denoiser and mark the guides, so the existing controls work.

## Scope

**In:**
- Fork commits: select OIDN; have hdMoonray mark albedo/normal guide outputs when guiding is on.
- A pin bump.
- Natron tooltips that describe the working controls.
- A render test showing the noise is reduced.

**Out:**
- Cycles path guiding.
- OptiX/XPU.
- New denoiser knobs. Labels and script names stay as they are.

**Edge cases:**
- Guiding on with no albedo/normal AOV requested: hdMoonray adds the guide outputs itself, and they are not shown as AOVs.
- OIDN missing at runtime: the render still succeeds and the log line says so.

**Assumptions:**
- The image's OpenImageDenoise (`build-moonray.sh` header line 16) is found by MoonRay's `mcrt_denoise` at configure time. P1 checks this.

## Phase 513.1: Trace

- [ ] M513.P1.T1 — Trace denoiser selection and guide inputs in the fork
  - files: read-only: a clone of the fork at the pinned commit under `build/` (moonray `mcrt_denoise`, the render driver's denoiser selection, scene variables, RenderOutput's denoiser-input attribute, hdMoonray's render settings and AOV handling), `tools/ci/local/build-moonray.sh`
  - approach: consultant (opus) names the code that picks OptiX over OIDN, where `enableOIDN` is read, whether OIDN is compiled in, and how a RenderOutput is marked as the albedo or normal guide and where hdMoonray creates outputs. It records the patch points per repository as a decision.
  - verify: a decision naming file:line for each patch point and the repositories touched
  - size: L

## Phase 513.2: Fork changes

- [ ] M513.P2.T1 — Select OIDN in MoonRay
  - files: fork repositories named by P1.T1 (moonray and/or hdMoonray), branch `natron/v2026.29.1`
  - approach: when denoising is on and OptiX is unavailable, or `enableOIDN` is set, use the OIDN denoiser on the CPU. Commit on the fork with an explanatory message.
  - verify: the fork builds with `build-moonray.sh` (PM runs the image build at the phase end)
  - size: L
- [ ] M513.P2.T2 — Mark albedo and normal guide outputs from hdMoonray
  - files: the fork's hdMoonray (per P1.T1)
  - approach: when `denoiseAlbedoGuiding` or `denoiseNormalGuiding` is on, create or flag the matching render outputs as denoiser inputs, without exposing them as user AOVs
  - verify: the same image build as P2.T1
  - size: M
- [ ] M513.P2.T3 — Bump the MoonRay pin
  - files: the fork super-repository (submodule pins), `tools/ci/local/Dockerfile:70`, `tools/ci/local/README.md` (pin note)
  - approach: point the super-repository at the P2 commits and set `NATRON_MOONRAY_COMMIT` to its new SHA, as README.md:130 describes
  - verify: the published image builds; `build-moonray.sh`'s closing library checks pass
  - size: S

## Phase 513.3: Natron

- [ ] M513.P3.T1 — Tooltips for working denoise controls
  - files: `Engine/Nodes/Scene/MoonRaySettings.cpp` (:122, :158-171)
  - approach: rewrite the group and knob tooltips to say what each control does now, and drop the "expect no effect" and "stays noisy" text. Keep `enableOIDN` following Enable Denoise, with a reason that matches the new behaviour. Never name other products.
  - verify: phase build; `ctest -R USDRenderMoonRay` (the settings audit, ~:1209) passes
  - size: M
- [ ] M513.P3.T2 — Test that denoising reduces the noise
  - files: `Tests/Scene/USDRenderMoonRay_Test.cpp` (the denoise test, ~:1180)
  - approach: render a flat-lit card at 2 samples a pixel with and without Enable Denoise, plus once with both guides on. Assert each render succeeds, the variance in a flat region drops by a clear factor with denoise on, and the log has no OptiX notice. Bundled gtest is old: no `GTEST_SKIP`, `INSTANTIATE_TEST_CASE_P` only, no `ASSERT_TRUE(ptr)`.
  - verify: `ctest -R USDRenderMoonRay` passes on the new image
  - size: M

**Verification gate:** the image is rebuilt on the new pin; `ctest -R "USDRenderMoonRay|MoonRayAovs"` green; full debug PR CI green on the new image.
