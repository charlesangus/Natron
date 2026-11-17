# M467 - LensFlare GPU Ghosts

Run LensFlare's ghost path on the GPU: the spectral ghost tracer, preprocessing, triangle weighting and compaction as Slang compute kernels, and the ghost rasterisation through M472 - GPU Raster Pipelines' hardware raster with additive RGBA32F blending — matching M466 - LensFlare Ghost Tracing's CPU path within tolerance. Ported from realflare's `raytracing.comp`, `preprocess.comp`, `flare_prim.comp`, `flare_commands.comp` and `flare.{vert,frag}`, with the per-ghost dispatch loop replaced by dispatches indexed by ghost.

## Scope

**In:**
- Slang kernels for the trace (every surface type of M462 - Shared Optics Library, coatings, per wavelength), preprocessing statistics, triangle weights with atomic index compaction, and indirect draw commands.
- Vertex and fragment stages for the ghost raster, with neighbour-averaged vertex intensity, sub-wavelength blending, the ghost-image lookup and spectral colour; 2x supersampling; wireframe.
- The ghost aperture image on the GPU (M465 - Aperture Model's kernel and M464 - FFT Library's GPU FFT).
- GPU placement for LensFlare, with the CPU path used when `rasterFloatBlend` is false or the GPU fails.

**Out:**
- The starburst on the GPU: M468 - LensFlare Starburst.
- A compute rasteriser: rejected (user, 2026-11-16).

**Edge cases:**
- Device without RGBA32F blend: CPU path, logged once.
- Preprocess statistics need a small readback per light (per-ghost means, CV² and bounds); the traced rays for the final pass stay on the GPU.
- Aspheres: the GPU uses all 8 coefficients like the CPU unless the parity test shows 4 suffice (M462 assumption).

## Phase 467.1: GPU trace

- [ ] M467.P1.T1 — Slang ghost tracer and preprocessing
  - files: `Engine/Gpu/Kernels/lensflare_trace.slang` (new), `Engine/Gpu/Kernels/lensflare_trace.cmake` (new), `Engine/Gpu/GpuLensFlare.{h,cpp}` (new), `Engine/Nodes/Generator/LensFlareGhosts.{h,cpp}`, `Tests/Native/NativeLensFlareGpu_Test.cpp` (new)
  - approach: Port `raytracing.comp` with M462's surface layout in a storage buffer (std430), the Ray record (position, direction, rrel, reflectance, stop position) and coatings. One dispatch per pass with the ghost index from the thread id, Y = wavelength. The preprocess pass (16 divisions, 1 wavelength) writes per-triangle area and intensity; a reduction writes per-ghost mean, CV² and bounds, read back once per light for M466's cull and division mapping.
  - verify: parity: traced sensor positions match M462's CPU tracer within 1e-4 mm for every built-in and two database aspheric lenses; preprocessing selects the same ghosts and divisions as the CPU; RADV at the phase build.
  - size: L

- [ ] M467.P1.T2 — Triangle weights, compaction and draw commands
  - files: `Engine/Gpu/Kernels/lensflare_prim.slang` (new), `Engine/Gpu/Kernels/lensflare_prim.cmake` (new), `Engine/Gpu/GpuLensFlare.{h,cpp}`, `Tests/Native/NativeLensFlareGpu_Test.cpp`
  - approach: Port `flare_prim.comp` (area, intensity, sliver factor, housing cull) and compact surviving triangles with an atomic counter into an index buffer; port `flare_commands.comp` to write one indexed indirect command per ghost with instances = wavelength sub-steps.
  - verify: parity: surviving triangle count and per-triangle intensities match M466's CPU weights within 1e-5 relative on the default lens.
  - size: M

## Phase 467.2: GPU raster

- [ ] M467.P2.T1 — Ghost raster through the graphics pipeline
  - files: `Engine/Gpu/Kernels/lensflare_raster.slang` (new), `Engine/Gpu/Kernels/lensflare_raster.cmake` (new, `slang_add_raster`), `Engine/Gpu/GpuLensFlare.{h,cpp}`, `Tests/Native/NativeLensFlareGpu_Test.cpp`
  - approach: Vertex stage pulls traced vertices, averages neighbour intensities and blends sub-wavelengths as `flare.vert`; fragment stage as `flare.frag` with the ghost image sampled from a buffer image. One `drawIndexedIndirect` per light into an RGBA32F attachment with additive blend (M472); supersampling renders at double size and a compute pass downsamples. Wireframe draws each triangle edge as a thin quad.
  - verify: parity against M466's CPU raster: per-pixel difference ≤ 1e-3 of the peak and total energy within 0.5% for the default lens, an off-axis light, supersampling and an isolated ghost; RADV and llvmpipe.
  - size: L

- [ ] M467.P2.T2 — GPU ghost aperture image
  - files: `Engine/Gpu/GpuLensFlare.{h,cpp}`, `Engine/Gpu/Kernels/lensflare_ghostimage.slang` (new, transfer function and normalisation), `Engine/Gpu/Kernels/lensflare_ghostimage.cmake` (new), `Tests/Native/NativeLensFlareGpu_Test.cpp`
  - approach: M465's aperture kernel → M464's GPU FFT → multiply by H → inverse → |·|² → normalise (max reduction) → vignette, all resident; cached on the GPU by the same key as the CPU.
  - verify: parity with M466.P2.T4 within 1e-4 of the peak at 1024².
  - size: M

## Phase 467.3: Placement

- [ ] M467.P3.T1 — LensFlare on the GPU with fallback
  - files: `Engine/Nodes/Generator/LensFlare.{h,cpp}`, `Tests/Native/NativeLensFlareGpu_Test.cpp`
  - approach: Add `gpu()` returning the GPU op as `CheckerBoardOp::gpu()` does, `GpuOpRef::none()` when `rasterFloatBlend` is false; the GPU trace cache mirrors the CPU one. A GPU failure falls back to the CPU path. Log the 2K default render time on the GPU next to the CPU time.
  - verify: gtest: with a GPU the node renders on it (placement counter) and matches the CPU within the P2.T1 tolerance; with `rasterFloatBlend` forced false it renders on the CPU; abort during a GPU render returns within 1 s.
  - size: M

**Verification gate:** build and ctest green with GPU parity on RADV and llvmpipe; the GPU and CPU 2K default render times recorded in Decisions; existing GPU suites unchanged.

## Decisions

- 2026-11-16 — **Hardware raster via M472 - GPU Raster Pipelines (user, after a pros/cons comparison):** parity with the CPU within tolerance, not bit-exact.
