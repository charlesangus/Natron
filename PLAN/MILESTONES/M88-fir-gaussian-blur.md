# M88 - FIR Gaussian Blur

Add a truncated FIR Gaussian filter to the native Blur node and make it the default for new nodes (user, 2026-10-10). The FIR Gaussian is the GPU-natural gather kernel M83 - GPU Compute Backend Spike prototypes, so matching it on the CPU means GPU and CPU Blur can agree within a tight tolerance in M85 - Native GPU Kernels. It also avoids the IIR filters' tiny far-field values that the Blur help text already warns about for premult–blur–unpremult. Existing projects must render exactly as before.

## Phase 88.1: FIR Gaussian filter

- [ ] M88.P1.T1 — Add the FIR Gaussian filter to native Blur and make it the default, keeping old projects unchanged
  - files: `Engine/Nodes/Filter/BlurKernels.h`, `Engine/Nodes/Filter/BlurKernels.cpp`, `Engine/Nodes/Filter/Blur.cpp`, `Engine/Nodes/Filter/Blur.h`
  - approach: Add `eFilterFIRGaussian` to `BlurKernels::Filter` and a separable two-pass FIR implementation: σ = size/2.4 (same mapping as the IIR Gaussian), radius ceil(3σ), weights computed in double and normalised to sum 1, accumulation in float or double — whichever matches M83.P3.T2's reference (truncated Gaussian, radius ceil(3σ), host-normalised weights, Neumann = clamp, Dirichlet = zero). Honour the existing boundary knob and the RoI expansion in `getRegionsOfInterest` / `getRoD` (FIR support is exactly the radius, so RoI padding is that radius). Append a new "Gaussian (FIR)" choice option with its own ID at the END of the Filter choice so saved indices/IDs keep meaning; change `setDefaultValue` to it. **Compatibility:** find how a Blur's Filter knob is serialized (`.ntp` — check whether default-valued knobs are omitted, and whether choices restore by ID or index). If an old project that never touched Filter would load with the new default, preserve its old behaviour — e.g. a project-version / node-version check that pins pre-change Blurs to the IIR Gaussian on load, following whatever precedent the codebase has for default changes. Update the help text/tooltips to describe the FIR Gaussian and the new default.
  - verify: builds; a loaded pre-change project (or a serialized Blur with default Filter, written by the old code) still renders IIR Gaussian; a new Blur defaults to FIR Gaussian.
  - size: L
- [ ] M88.P1.T2 — Tests for the FIR Gaussian Blur
  - files: a new `Tests/BlurFIRGaussian_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: Render an impulse and a step through native Blur with the FIR filter: impulse response matches a double-precision truncated Gaussian within 1e-6, exactly zero beyond the radius, sums to 1 (Neumann) on an interior impulse; σ sweep {0.5, 3, 25}; boundary modes; RoI correctness (rendering a sub-rect equals cropping a full render). Plus a compatibility test: a Blur restored from a serialization with the pre-change default renders the IIR Gaussian.
  - verify: `ctest` green, including the new test.
  - size: M

**Verification gate:** `ctest` green including `BlurFIRGaussian_Test`; new Blur nodes default to FIR Gaussian; an existing project with a default-filter Blur renders byte-identically to before.

## Decisions

- 2026-10-10 — Split out of M83 - GPU Compute Backend Spike so a production node change doesn't ride in a spike PR, and so it runs in its own lane in parallel. Runs off `main`, PR against `main`.
