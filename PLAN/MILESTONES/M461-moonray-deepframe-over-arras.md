# M461 - MoonRay DeepFrame Over Arras

Carry MoonRay's resolved deep buffer from the out-of-process Arras render into Natron. A new DeepFrame message is sent after the FINISHED progressive frame and before `stopFrame`, serialised from `RenderContext::getDeepBuffer()` in 64-row bands so no message nears the 4 GiB buffer limit or the chunking copy; hdMoonray assembles the bands into the `HydraDeepBuffer` M286 - Utility Renderer Deep Output defines and serves it from `HdRenderBuffer::GetResource()`. M255 - Delegate Output Spikes measured about 31 MiB per 1080p frame, 1.8 MiB per band (`spike/m255/REPORT-P2T2-moonray-deep-arras.md`, 852f31da2). Estimate from the 2026-11-16 costing: most of the 900–1,400 fork lines across mcrt_messages, mcrt_computation and hdMoonray.

## Scope

**In:**
- New charlesangus forks of mcrt_messages and mcrt_computation, wired into the openmoonray pin and build script.
- The DeepFrame message, the banded send with stop checks, and hdMoonray's band assembly into `HydraDeepBuffer`.

**Out:**
- The true-deep presence and volume patch: M460 - MoonRay True Deep Patch.
- USDRender wiring and comp tests: M258 - MoonRay Deep Output.
- Deep for progressive passes and multi-machine merge (`progmcrt_merge`).
- DCX masks in the message.

**Edge cases:**
- Opacity Only: only A/Z/ZBack are sent.
- Stop requested mid-transfer: checked between bands; the render ends within 1 s.
- Non-final progressive passes: no DeepFrame is sent.

## Phase 461.1: DeepFrame over Arras

- [ ] M461.P1.T1 — DeepFrame message
  - files: a new charlesangus fork of mcrt_messages, the openmoonray `.gitmodules` and pin, `tools/ci/local/build-moonray.sh`
  - approach: header (frame id, size, aperture/region, channel names, band index and count, row range) and body (uint32 counts per band row, packed float32 samples in Z order); no DCX masks.
  - verify: a fork unit test round-trips a 3-band frame bit-exactly.
  - size: M

- [ ] M461.P1.T2 — Send resolved deep in bands
  - files: a new charlesangus fork of mcrt_computation (progressive frame send)
  - approach: after the FINISHED frame, before `stopFrame`, resolve `getDeepBuffer()` with `DeepBuffer::write`'s normalisation and send 64-row bands; opacity-only sends A/Z/ZBack; stop is checked between bands; nothing is sent for non-final passes.
  - verify: the `mr_shim.cpp` shim from the spike sees bands equal to the batch DeepBuffer; a stop mid-transfer ends within 1 s.
  - size: L

- [ ] M461.P1.T3 — hdMoonray assembles HydraDeepBuffer
  - files: charlesangus/hdMoonray (render buffer, Arras receive), vendored `HydraDeepBuffer.h`, `tools/ci/local/Dockerfile`
  - approach: needs M286.P2.T2's header, vendored byte-identical with its layout version. Binding `deep` sets `output_type=deep` and `deep_z_tolerance`; bands assemble into counts, offsets and channels, served by `GetResource()` once complete. Publish the image and recreate natron-dev.
  - verify: the spike's probe on the new image: flattened deep vs Hydra colour RMSE ≤ 1e-4 (with M460's patch in the image).
  - size: L

**Verification gate:** the fork unit test, the shim and the probe pass on the published image; existing MoonRay ctest green on it; band count and bytes logged for a 1080p frame.

## Decisions

- 2026-11-16 — **Split from M258 - MoonRay Deep Output (user, inbox and grill).** Transport per `PLAN/DECISIONS/2026-11-16-moonray-deep-over-arras.md`.
