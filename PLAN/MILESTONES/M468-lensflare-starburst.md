# M468 - LensFlare Starburst

Add the diffraction starburst to LensFlare: the Fraunhofer far field of the starburst aperture (its power spectrum, `fftshift(fft2(aperture))`, |F|²/N²), sampled per pixel with spectral Monte Carlo jitter — each sample at a wavelength in 390–730 nm, scaled by λ/560, blurred and rotated — then tinted by the wavelength's colour and weighted by (560/λ)² (Ritschel 2009, Kakimoto 2005, Hullin 2011). Ported from realflare's `tasks/starburst.py` and `shaders/starburst.frag`, on CPU and GPU with parity, added onto the ghosts of M466 - LensFlare Ghost Tracing.

## Scope

**In:**
- Starburst tab: M465 - Aperture Model's knob group with prefix `starburstAperture`; `starburstScaleWithFStop` (off), `starburstScale` (1), `starburstIntensity` (1), `starburstBlur` (0), `starburstRotation` (0°, 0–180), `starburstRotationWeight` (1, 0–2), `starburstVignetting` (1), `starburstResolution` (choice 256–4096, 1024), `starburstSamples` (7; 2^n samples, up to 12). An optional "Starburst Aperture" input feeds its image layer.
- The far-field texture with mipmaps, CPU (M464 - FFT Library's pocketfft) and GPU (its Slang FFT), cached per aperture; aperture parallax makes it per light when dust or scratch parallax is non-zero.
- The per-pixel sampler on CPU and GPU, with a PCG hash so both draw the same samples.
- Per light: centred on the light position, intensity × illuminant × tint.

**Out:**
- Output layers: M469 - LensFlare Layers And Diagram adds `starburst` and `starburstAperture`.
- realflare's starburst `occlusion` flag: hidden and unused there, not ported. Occlusion by a depth pass is M470 - LensFlare From USD Lights.
- realflare's ×10 radius and blur/100 factors: the radius is derived from the diffraction maths (λ × f-number × FFT size / sensor), and knob defaults are tuned to realflare's look (user, 2026-11-16).

**Edge cases:**
- `starburstIntensity` 0: no FFT and no sampling.
- `starburstScaleWithFStop` off: an f-number of 2.8 is used for the size, as in realflare.
- Samples falling off the FFT texture are faded by `starburstVignetting`'s smooth vignette, hiding the texture edge.
- Mip level matched to the pixel footprint, so large scales don't alias.

## Phase 468.1: Starburst

- [ ] M468.P1.T1 — Starburst knobs and far-field texture (CPU)
  - files: `Engine/Nodes/Generator/LensFlare.{h,cpp}`, `Engine/Nodes/Generator/LensFlareStarburst.{h,cpp}` (new), `Tests/Native/LensFlareStarburst_Test.cpp` (new)
  - approach: Add the Starburst tab knobs and the optional input. Render the `starburstAperture` mask (M465) at `starburstResolution`, take M464's CPU power spectrum with fftshift, and build float mipmaps; cache by aperture parameters (plus the light position when parallax is non-zero).
  - verify: gtest: a 6-blade aperture's spectrum has 6-fold (12 spikes) symmetry within 1% energy per sector; a disc gives the Airy first dark ring at the analytic radius within one texel; identical parameters hit the cache.
  - size: M

- [ ] M468.P1.T2 — Spectral sampler and compositing (CPU)
  - files: `Engine/Nodes/Generator/LensFlareStarburst.{h,cpp}`, `Engine/Nodes/Generator/LensFlare.cpp`, `Tests/Native/LensFlareStarburst_Test.cpp`
  - approach: Port `starburst.frag`: per pixel, 2^`starburstSamples` samples from a PCG hash of the pixel and sample index; wavelength uniform in 390–730 nm; position (pixel − light) mapped to texture space by the derived radius × `starburstScale`, jittered in a disc by `starburstBlur`, scaled by λ/560, rotated by a random angle within ±`starburstRotation` weighted by `starburstRotationWeight`; trilinear mip lookup; × vignette × the wavelength's working-space colour (M462) × (560/λ)²; averaged, × intensity, illuminant and tint; added to the ghosts. Multi-threaded by rows.
  - verify: gtest: the result is deterministic; doubling `starburstScale` doubles the first-ring radius within one pixel; `starburstRotation` 0 with blur 0 gives a symmetric pattern about the light; total energy scales linearly with intensity.
  - size: M

- [ ] M468.P1.T3 — Starburst on the GPU
  - files: `Engine/Gpu/Kernels/lensflare_starburst.slang` (new), `Engine/Gpu/Kernels/lensflare_starburst.cmake` (new), `Engine/Gpu/GpuLensFlare.{h,cpp}`, `Tests/Native/NativeLensFlareGpu_Test.cpp`
  - approach: M465's GPU aperture → M464's GPU FFT → power spectrum and fftshift → mip chain, resident and cached; the sampler as one compute pass with the same PCG hash and maths, adding into M467's flare image. Follow the existing `.cmake` registration pattern.
  - verify: parity with P1.T2 within 1e-3 of the peak at 2K with 7 sample bits, on RADV and llvmpipe.
  - size: M

- [ ] M468.P1.T4 — Starburst goldens and docs
  - files: `Tests/Native/goldens/LensFlare/` (starburst crops), `Engine/Nodes/Generator/docs/LensFlare.md`, `docs/lensflare/controls-audit.md`
  - approach: Goldens for the default starburst, a dusty aperture with parallax, and scale with f-stop on. Document the starburst controls; fill the audit's starburst rows.
  - verify: `ctest -R "LensFlareStarburst|NativeLensFlare"` passes; the audit has no MISSING starburst rows.
  - size: M

**Verification gate:** build and ctest green with GPU parity on RADV and llvmpipe; starburst screenshots under Xvfb shared with the user (collected at F31 - Lens Flares' UAT).

## Decisions

- 2026-11-16 — **Derived sizes, realflare look (user):** no hidden ×10 or /100 factors; defaults tuned to match realflare's default project.
