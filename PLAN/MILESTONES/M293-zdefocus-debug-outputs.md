# M293 - ZDefocus Debug Outputs

Diagnostic views on ZDefocus, taken from Niik-l's LensDefocus Output tab (`niikl/RB-2.6:Engine/Dev/PostFX/LensDefocus.cpp` ~:1218) at the user's request (2026-11-01). ZDefocus's `output` choice (`kZDefocusParamOutput`, today Result and Focal Plane) gains:
- **Blur Radius:** R is the signed CoC in pixels (negative in front of focus), G is |CoC|, and B is the CoC after the maxSize clamp.
- **Slices:** the depth layer each pixel's CoC falls in, in false colour, so the layer count and spacing can be judged.

Focal Plane already plays the part of Niik-l's Focus Map. Edge Split, Surface Edges and the `defocusError` plane describe Niik-l's edge-split and fill stages, which only exist if M294 - ZDefocus Engine Revisit adopts them.

Deps: M131 - Native Defocus Family, M133 - ZDefocus Lens Aberrations.

## Scope

**In:**
- Blur Radius and Slices output modes on ZDefocus, on CPU and GPU, with tests and docs.

**Out:**
- Edge Split, Surface Edges and the `defocusError` plane: M294 - ZDefocus Engine Revisit, if it adopts edge split and fill.
- Debug outputs on Defocus: it has a uniform size, so there is nothing to show.

**Edge cases:**
- **Debug modes write RGB and set alpha to 1.** Mask and mix don't apply, so the view shows raw data.
- **Blur Radius at render scale < 1:** values are in full-resolution pixels, so the readout doesn't change with proxy.
- **Slices with per-channel CoC (M133 CA):** uses the green CoC, as ZDefocus's coverage does.

**Assumptions:**
- Unconfirmed: the Slices palette cycles 8 distinct hues with the in-focus layer drawn white.

## Phase 293.1: Debug outputs

- [ ] M293.P1.T1 — Blur Radius and Slices on CPU
  - files: `Engine/Nodes/Filter/ZDefocus.h`, `Engine/Nodes/Filter/ZDefocus.cpp`
  - approach: Add the choice options `blurRadius` ("Blur Radius") and `slices` ("Slices") to `kZDefocusParamOutput`. Both are evaluated per pixel from the depth-to-CoC model already used for the gather (`DepthOfField.h`), with no gather. Slices uses the same layer assignment as the gather. Return early, before the gather.
  - verify: `Tests/Native/NativeZDefocus_Test.cpp` cases: Blur Radius at a known depth equals `DepthOfField.h`'s CoC; R is negative for depth nearer than focus; Slices assigns adjacent depths to the gather's layers.
  - size: M

- [ ] M293.P1.T2 — Blur Radius and Slices on GPU
  - files: `Engine/Gpu/Kernels/zdefocus.slang`, `Engine/Nodes/Filter/ZDefocus.cpp`
  - approach: Add an entry point that writes the two modes from the depth plane and the CoC parameters already in the metadata buffer. Dispatch it in place of the gather when a debug mode is selected.
  - verify: `Tests/Native/NativeZDefocusGpu_Test.cpp` parity cases for both modes pass on RADV.
  - size: M

- [ ] M293.P1.T3 — Document the debug outputs
  - files: `Engine/Nodes/Filter/docs/ZDefocus.md`
  - approach: Describe each output mode, its channel layout and how to read it: picking the layer count from Slices, and spotting clamped areas in Blur Radius.
  - verify: the doc lists all four output modes with their channel meanings.
  - size: S

**Verification gate:** the full build and `ctest` pass, with GPU parity on RADV. Under Xvfb, ZDefocus renders a CG fixture with a depth pass in each output mode, and the screenshots are shared with the user before sign-off.

## Decisions

- 2026-11-01 — **Debug outputs requested (user, /cat-plan):** out of Niik-l's LensDefocus feature list, the user picked the debug outputs. Only the modes meaningful for today's layered gather land here; the rest wait on M294 - ZDefocus Engine Revisit.
