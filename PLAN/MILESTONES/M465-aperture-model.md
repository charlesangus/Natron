# M465 - Aperture Model

A shared procedural aperture (diaphragm) mask on CPU and GPU, ported from realflare's `aperture.frag`: a polygon of blades with roundness, rotation and softness, multiplied by grating, scratches, dust and an image layer. LensFlare uses it for its ghost and starburst apertures (F31 - Lens Flares); M292 - Traced Lens Bokeh uses it for the bokeh iris. Includes a knob-group helper so every node exposes the same controls.

Source: realflare (GPL-3.0-or-later) `flare/engine/tasks/shaders/aperture.frag`, `flare/engine/tasks/aperture.py`, `flare/api/project/model.py` (Aperture form).

## Scope

**In:**
- `Optics::ApertureParams` and an aperture rasteriser on the CPU and as a Slang kernel, 8×8 supersampled per pixel.
- Every realflare aperture control, as a prefixed knob group (`<prefix>Size`, …):
  - Shape: size (0.75, 0.75, ratio-locked), blades (6, min 2), roundness (0), rotation (0°), softness (0).
  - Grating: strength 0, density 0.5, length 0.5, width 0.25, softness 0.
  - Scratches: strength 0, density 0.5, length 0.5, width 0.25, rotation 0, rotation variation 0, softness 0, parallax (1, 1).
  - Dust: strength 0, density 0.5, radius 0.5, softness 0, parallax (1, 1).
  - Image: strength 0, size (0.75, 0.75), black 0, white 1, from an optional image the caller supplies.
- Parallax: dust and scratches offset by a caller-supplied light position × parallax.

**Out:**
- Using it in nodes: LensFlare (M466 - LensFlare Ghost Tracing, M468 - LensFlare Starburst) and bokeh (M292 - Traced Lens Bokeh).
- The image layer's source: callers supply it (an optional node input).

**Edge cases:**
- Every layer strength 0: the mask is the polygon alone.
- realflare's sine hash makes dust and scratch placement depend on `sin` precision; this port uses a PCG hash, so CPU and GPU place them identically.
- Width and radius scale ×0.1 and softness ÷10 internally, as realflare does, so knob ranges match realflare's.

## Phase 465.1: Aperture

- [x] M465.P1.T1 — CPU aperture rasteriser
  - files: `Engine/Optics/Aperture.{h,cpp}` (new), `Tests/Native/OpticsAperture_Test.cpp` (new)
  - approach: Port `aperture.frag`'s SDF polygon (half-planes per blade, roundness as a sin(π·blade gradient) bulge, softness as smoothstep), grating (up to 360 radial bars), scratches and dust (up to 1000 rectangles and circles each, placed from a PCG stream), and the image layer (sampled, clamped between black and white). Each layer multiplies as 1 − clamp(Σ·strength, 0, 1). 8×8 supersampling. Multi-threaded by rows. Output: a single-channel float mask of a requested size.
  - verify: gtest: 6 blades, roundness 0, at 512² is a hexagon whose area matches the analytic value within 0.5%; a large blade count approaches a disc; dust strength 1 lowers the mean; the same parameters give the same mask twice.
  - size: M

- [x] M465.P1.T2 — GPU aperture kernel
  - files: `Engine/Gpu/Kernels/aperture.slang` (new), `Engine/Gpu/Kernels/aperture.cmake` (new), `Engine/Gpu/GpuAperture.{h,cpp}` (new), `Tests/Native/OpticsApertureGpu_Test.cpp` (new)
  - approach: The same maths in Slang with the same PCG stream; dust and scratch lists are built on the CPU and passed in a storage buffer so both sides share them. Follow the `blur.cmake` registration pattern; host wrapper like `GpuBlur.h`.
  - verify: GPU parity with P1.T1 within 1e-5 per pixel for the polygon, grating, scratches, dust and image cases, on RADV at the phase build (`reportNoGpu` skips without a device).
  - size: M

- [x] M465.P1.T3 — Aperture knob group
  - files: `Engine/Optics/ApertureKnobs.{h,cpp}` (new), `Tests/Native/OpticsAperture_Test.cpp`
  - approach: `ApertureKnobs::create(effect, page, prefix, labelPrefix, options)` adds the Scope's knobs (script names `<prefix>Size`, `<prefix>Blades`, `<prefix>GratingStrength`, …; labels from the script names) in collapsible groups Shape, Grating, Scratches, Dust, Image; `options` can omit knobs a caller already has (bokeh's blades and rotation). `params(time)` returns `ApertureParams`.
  - verify: gtest: a test effect gets every knob with realflare's defaults; `params()` reflects edits; omitting blades leaves them to the caller.
  - size: M

**Verification gate:** build and `ctest -R OpticsAperture` green with GPU parity on RADV; a contact of the default, grating, scratches and dust masks is rendered by the test and shared with the user as separate PNGs (collected at F30 - Lens Optics Base's UAT).

## Decisions

- 2026-11-16 — **Aperture model in the shared base, used by bokeh too (user):** moved into F30 - Lens Optics Base so M292 - Traced Lens Bokeh draws its iris with it.
- 2026-11-20 — All tasks were found committed (f752d453a, 52d4c9ec7) but unchecked. Remote gate green on 52d4c9ec7 (run 44767931769). RADV parity and the contact PNGs are collected at F30 - Lens Optics Base's UAT. PR into F30 next.
- 2026-11-20 — Review round 1 (12 findings, PR #323) fixed in 7692216b5, apart from two:
  - #9 declined: `GpuKernel::bindAndDispatch` already checks the group limits.
  - #1 can't be bit-exact: the gradient's division and Vulkan's sqrt aren't correctly rounded. The hard rounded case allows one sample per pixel on at most 16 of 7680 pixels; every other case keeps 1e-5.

  **Size and Image Size get no ratio lock (user):** the knob framework has none, and auto-fold is enough, so that part of the Scope is dropped. Gate re-running.
- 2026-11-20 — Merged into feature/f30-lens-optics-base (bc99ce0af, #323) after review round 1, with PR CI green on fa5cd9f1a.
