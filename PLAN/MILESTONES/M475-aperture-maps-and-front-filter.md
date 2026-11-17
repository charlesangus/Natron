# M475 - Aperture Maps And Front Filter

Art in the iris and dirt or a matte box in front of the lens, for ZDefocus and Defocus (Higx Optics parity, user 2026-11-16). A new optional **Aperture** input carries an RGB iris image, traced through the lens. Per-channel transmission gives colour fringing. A smooth inner iris and iris softness keep discs clean. A new optional **Front Filter** input, or a built-in Round Filter or Square Mattebox shape, sits on the front element or at the stop. Each position's bokeh reads the filter through the patch its own rays cross, sampled exactly at the traced rays' footprints. A Pupil Phase mode treats the filter image as surface shape that bends light, through M474 - Traced Lens Creative Aberrations's pupil terms.

Deps: M292 - Traced Lens Bokeh, M474 - Traced Lens Creative Aberrations (Pupil Phase uses its sensor-plane perturbation).

## Scope

**In:**
- M465 - Aperture Model's image layer extended to RGB (three masks), plus `irisSize` (1 = the glass draws the rim, below 1 a smooth circular iris inside it) and the map's fit and rotation. Single-channel callers are unchanged.
- An optional `Aperture` input on ZDefocus and Defocus (user, 2026-11-16: a new input, not the Kernel input). Knobs, with labels from the script names: `apertureMapFit` (1: the map's square just contains the iris circle), `apertureRotation` (degrees), `irisSize` (1), `irisSoftness` (0: fades transmission inward from the iris edge as a fraction of its radius). A traced lens traces the map as its stop's transmission. A procedural lens uses it as the kernel shape, intersected with blades when Kernel Type is Blades. The map never changes overall brightness except in Absolute vignetting (M292), where its tint and density darken the frame.
- An optional `FrontFilter` input and knobs: `frontFilterShape` (Off, Image, Round Filter, Square Mattebox), `frontFilterSize`, `frontFilterAtStop` (off: on the front element), `frontFilterParallax` (frame height = 1; with At Stop off, the filter's distance in front-element diameters), `frontFilterShading` (1: full physical falloff in the physical vignetting modes; 0: shape only), `frontFilterMode` (Transmission, Pupil Phase) and `phaseStrength` (mm of sensor travel at the iris edge for a full black-to-white ramp; its sign flips the direction).
- Traced: the bake records each ray's hit on the first surface (normalised by its clear semi-diameter), or its stop position with At Stop on, and weights it by the filter there, offset by field × parallax. Procedural: the patch shifts by the cell's position × parallax.
- CPU and GPU, goldens, docs and a GUI check.

**Out:**
- Kernel Type = Image keeps meaning "this exact bokeh", with no tracing (user, 2026-11-16).
- Glitter and other per-highlight sparkle layers: not wanted (user, 2026-11-16).
- Aperture parallax from a light position: bokeh has none (M292's rule for M465's parallax).

**Edge cases:**
- Inputs disconnected and the shape Off: bit-identical to M474's output.
- An all-black aperture map or filter (no light passes): a node error saying the aperture blocks all light, with the output passed through.
- A non-square map: fit by its longer side, centred.
- An animated Aperture or FrontFilter input: the cache key includes a hash of the map, downsampled to the bake's resolution, so a changed frame bakes again and an unchanged one hits the cache.
- Pupil Phase with a flat (constant) map: no change. The filter's own darkening is off in that mode.
- A map at render scale < 1: the map is fetched at the scale that gives the bake's resolution, independent of the render scale.

**Assumptions:**
- Unconfirmed: a 256² map resolution in the bake is enough for dust and scratch detail at bokeh sizes up to 500 px.
- Unconfirmed: the inputs are fetched once per render at the frame's time, like the Kernel input.

## Phase 475.1: Aperture map

- [ ] M475.P1.T1 — RGB image layer, iris size and map fit in the aperture model
  - files: `Engine/Optics/Aperture.{h,cpp}`, `Engine/Optics/ApertureKnobs.{h,cpp}`, `Engine/Gpu/Kernels/aperture.slang`, `Engine/Gpu/GpuAperture.{h,cpp}`, `Tests/Native/OpticsAperture_Test.cpp`, `Tests/Native/OpticsApertureGpu_Test.cpp`
  - approach: Add an RGB variant of the image layer that returns three masks (each layer multiplies per channel). Add `irisSize` (a smoothstep-edged disc of that radius fraction, multiplied in) and map fit and rotation, defaulting to today's output. Keep the single-channel API as is for LensFlare.
  - verify: gtest: defaults are bit-identical to M465's masks; a red-only image gives zero green and blue masks; irisSize 0.5 halves the disc radius within one pixel at 512²; GPU parity within 1e-5.
  - size: M

- [ ] M475.P1.T2 — Aperture input on ZDefocus and Defocus
  - files: `Engine/Nodes/Filter/ZDefocus.{h,cpp}`, `Engine/Nodes/Filter/Defocus.{h,cpp}`, `Engine/Nodes/Filter/LensTable.{h,cpp}`, `Engine/Nodes/Filter/LensTableCache.{h,cpp}`, `Engine/Nodes/Filter/BokehKernel.{h,cpp}`, `Tests/Native/NativeZDefocus_Test.cpp`, `Tests/Native/NativeDefocus_Test.cpp`
  - approach: Add the optional `Aperture` input and the Scope's aperture knobs. Fetch the map at the bake resolution, build the RGB mask through M475.P1.T1, and pass it to the bake as per-ray, per-channel transmission at the stop. Procedural lenses rasterise the mask as the kernel (intersected with blades for Blades). Normalise each channel's kernel to unit sum. Add the map hash to the cache key.
  - verify: tests: a disconnected input is bit-identical; a white map with Disc equals the Disc kernel within 1e-6; a red-only map gives a red-only kernel; apertureRotation 90 rotates the kernel; an all-black map gives the node error and identity output.
  - size: L

## Phase 475.2: Front filter

- [ ] M475.P2.T1 — Front-element footprints in the bake
  - files: `Engine/Optics/Trace.{h,cpp}`, `Engine/Nodes/Filter/LensTable.{h,cpp}`, `Engine/Gpu/Kernels/opticsTrace.slang`, `Engine/Gpu/Kernels/lenstable.slang`, `Tests/Native/LensTable_Test.cpp`
  - approach: The tracer optionally returns each ray's first-surface hit, normalised by that surface's clear semi-diameter. The bake weights each ray by the filter sampled at (hit × size + field × parallax), or at the stop position with At Stop on. Round Filter and Square Mattebox are analytic: a clear disc with a soft ring, and a clear square. In the physical vignetting modes, mean transmission feeds illumination, scaled by shading.
  - verify: tests: a constant filter changes nothing; a dark-ringed round filter clips corner kernels into cat-eyes while the centre kernel is unchanged; parallax 0 gives every cell the same patch; a square mattebox gives square-clipped corner kernels; CPU and GPU bakes agree.
  - size: L

- [ ] M475.P2.T2 — Front Filter input and knobs
  - files: `Engine/Nodes/Filter/ZDefocus.{h,cpp}`, `Engine/Nodes/Filter/Defocus.{h,cpp}`, `Engine/Nodes/Filter/BokehKernel.{h,cpp}`, `Tests/Native/NativeZDefocus_Test.cpp`
  - approach: Add the optional `FrontFilter` input and the Scope's knobs in a "Front Filter" group. Shapes override a connected input. Procedural lenses shift the filter patch by the cell position × parallax and multiply it into the kernel. Hash the filter into the cache key.
  - verify: tests: Off is bit-identical; Image with a disconnected input behaves as Off; a procedural render with a dark-left filter darkens the left of right-side kernels more as parallax grows.
  - size: M

- [ ] M475.P2.T3 — Pupil Phase mode
  - files: `Engine/Optics/PupilTerms.{h,cpp}`, `Engine/Nodes/Filter/LensTable.{h,cpp}`, `Engine/Gpu/Kernels/opticsTrace.slang`, `Engine/Nodes/Filter/ZDefocus.cpp`, `Engine/Nodes/Filter/Defocus.cpp`, `Tests/Native/LensTable_Test.cpp`
  - approach: In Pupil Phase mode the filter adds Δz = phaseStrength × (map − 0.5) at each ray's stop position (a sampled term added to M474's `PupilTerms`) and contributes no transmission or darkening. Traced lenses only; disabled for Procedural.
  - verify: tests: a constant map is bit-identical to Transmission with the filter Off; a left-to-right ramp shifts the kernel's centroid by the amount the tracer predicts; kernel sums stay 1 within 1e-6; CPU and GPU agree.
  - size: M

## Phase 475.3: Docs and GUI

- [ ] M475.P3.T1 — Goldens, docs and GUI check
  - files: `Tests/Native/goldens/ZDefocus/` (aperture and filter goldens), `Engine/Nodes/Filter/docs/ZDefocus.md`, `Engine/Nodes/Filter/docs/Defocus.md`, `Tests/gui/m475_aperture_filter.py` (new), small synthetic maps generated by the test (none committed beyond ≤256 px)
  - approach: Goldens for an RGB aperture map, a dirt filter on the front element, a square mattebox and Pupil Phase, through Double-Gauss on a point-highlight grid. Docs cover both inputs, every knob and the interaction with Kernel Type. Never name other products. The GUI script connects generated maps and screenshots each case.
  - verify: `ctest -R "NativeZDefocus|NativeDefocus|OpticsAperture|LensTable"` passes; the GUI script passes, and its screenshots are collected for F13 - ZDefocus Rework's UAT.
  - size: M

**Verification gate:** the full build and `ctest` pass, with GPU parity on RADV; disconnected inputs with the shape Off are bit-identical to M474 - Traced Lens Creative Aberrations; the GUI screenshots are collected at F13 - ZDefocus Rework's UAT.

## Decisions

- 2026-11-16 — **All of it, traced exactly (user, /cat-plan grill):** RGB aperture maps, iris size and softness, the front filter at real front-element footprints, the built-in shapes and Pupil Phase.
- 2026-11-16 — **A new Aperture input (user):** the Kernel input keeps its meaning; Kernel Type = Image bypasses the lens.
