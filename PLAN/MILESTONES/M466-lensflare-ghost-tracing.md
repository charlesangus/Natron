# M466 - LensFlare Ghost Tracing

A native LensFlare generator that renders physically based lens-flare ghosts by tracing light through a real lens prescription, ported from realflare (`https://github.com/beatreichenbach/realflare`, GPL-3.0-or-later, v1.0.3 at 9ce30b0). Every ghost is a pair of reflecting surfaces. A hexagonal ray grid on the entrance surface is traced per wavelength through each surviving ghost, its triangles are weighted by how much they shrink and their two-bounce reflectance (with thin-film coatings), and they are rasterised additively with a diffracted ghost-aperture image and spectral colour (Hullin et al. 2011). This milestone delivers the node, its lights and every flare- and ghost-tab control, and the full CPU path, which is also the reference for M467 - LensFlare GPU Ghosts. The starburst is M468 - LensFlare Starburst; output layers and the lens diagram are M469 - LensFlare Layers And Diagram.

Port sources: `flare/engine/tasks/common/raytracing.py`, `common/disc.py`, `tasks/preprocess.py`, `tasks/flare.py`, `tasks/ghost.py`, `tasks/shaders/{preprocess,raytracing,flare_prim}.comp`, `tasks/shaders/flare.{vert,frag}`, `flare/api/project/model.py`. Built on F30 - Lens Optics Base (M462 - Shared Optics Library, M463 - Bundled Lens Database, M465 - Aperture Model) and M464 - FFT Library.

## Scope

**In:**
- `LensFlare`: a pure generator (no Source input) on `NativeGenerator` with the usual extent knobs; output RGB is emitted light, alpha 0. An optional "Ghost Aperture" input feeds the ghost aperture's image layer.
- Lights: "Add Light" and "Remove Light" buttons create and remove numbered groups of real, animatable knobs — `light<N>Enabled` (on), `light<N>Position` (2D, pixels in the format, a viewer handle each), `light<N>Intensity` (1), `light<N>Illuminant` (choice of M462's CIE illuminants, D65), `light<N>Tint` (colour, 1). Light 1 exists by default. Positions may be outside the format.
- Lens tab: M463's shared lens knobs (`lensModel`: Database, From File; `lens`, default the Nikon AF Zoom-Nikkor 28-85mm f3.5-4.5 as in realflare's default project; `lensFile`, `glassVendor`, `abbeOffset`, `useAspheric`); `sensorSize` (36 × 24 mm); `fStop` (2.8).
- Coatings: `coatingSeed` (0), a `randomizeCoatings` button that picks a new seed, `coatingWavelengthRange` (390, 730 nm), `coatingIorRange` (1.38, 2.2), and a `coatings` table (Surface, Wavelength, IOR) filled from the seed and editable.
- Raytracing: `wavelengths` (5), `subWavelengths` (1), `minDivisions` (16), `maxDivisions` (64), `minArea` (0.01), `minSliver` (0.1), `cullPercentage` (0.5), `supersampling` (None, 2x).
- Ghost tab: M465's aperture knob group with prefix `ghostAperture` (realflare's ghost default of 64 blades), `ghostDistance` (2), `ghostVignette` (0.1), `ghostResolution` (choice 256–4096, 1024).
- Debug: `isolateGhost` (off) with `ghost` (0), `wireframe` (off).
- Labels follow script names throughout.

**Out:**
- GPU path: M467 - LensFlare GPU Ghosts.
- Starburst and its knobs: M468 - LensFlare Starburst.
- Output choice, extra layers and the lens diagram: M469 - LensFlare Layers And Diagram.
- USD lights and a depth input: M470 - LensFlare From USD Lights.
- Browser previews and thumbnails: M471 - Lens Browser Previews.
- realflare's output path, write, frame, render-to-disk and animation JSON: Natron's Write node and animated knobs cover them.
- realflare's hidden, unused starburst `occlusion` flag: not ported.
- realflare's fudge factors (ghost screen scale ×0.8): sizes are derived from the sensor and the lens (user, 2026-11-16); defaults are tuned so a default render looks like realflare's default project.

**Edge cases:**
- No light enabled, or every light's intensity 0: black, no tracing.
- A light far outside the frame: traced as usual; ghosts that land in frame render.
- Lens fails to resolve (missing id, bad file): node error, black output.
- A ghost with no valid rays after preprocessing: skipped.
- Caustic edges: triangles collapse and brighten; `minArea` floors the area and `minSliver` fades slivers, as in realflare.
- realflare's vertex shader compares an unsigned index with `< 0` when averaging neighbours (always false); the port averages the valid neighbours as intended, not the bug.
- Render scale (proxy): the trace is identical; the raster runs at the scaled resolution.
- Abort: checked between ghosts and raster bands; returns promptly.

**Assumptions:**
- Unconfirmed: dynamic light groups are recreated on project load from a hidden `lightCount` knob before values restore, the same way for Python-created lights.
- Unconfirmed: the CPU path renders a final-quality single light (5 wavelengths, 64 max divisions) at 2K in a few seconds on 8 cores.

## Phase 466.1: Node and knobs

- [ ] M466.P1.T1 — LensFlare node with lens, camera, raytracing, ghost and debug knobs
  - files: `Engine/Nodes/Generator/LensFlare.{h,cpp}` (new), `Engine/Nodes/Generator/LensFlare.reg.inc` (new), `Engine/Nodes/Generator/docs/LensFlare.md` (new, stub), `Engine/CMakeLists.txt`
  - approach: Follow `CheckerBoard` (a `NativeGenerator` with extent knobs, `NATRON_NATIVE_NODE` registration). Create the Lens tab knobs with `LensKnobs` (M463), the camera, raytracing and debug knobs, and the Ghost tab with `ApertureKnobs::create(…, "ghostAperture", …)` (M465) plus `ghostDistance`, `ghostVignette`, `ghostResolution`, all with the Scope's defaults and ranges. Declare the optional "Ghost Aperture" input. Render black for now.
  - verify: `Tests/Native/NativeLensFlare_Test.cpp` (new): the node instantiates with every knob at its default, the generator's RoD follows its extent, and the output is black with alpha 0.
  - size: M

- [ ] M466.P1.T2 — Dynamic light groups with viewer handles
  - files: `Engine/Nodes/Generator/LensFlare.{h,cpp}`, `Tests/Native/NativeLensFlare_Test.cpp`
  - approach: `addLight()`/`removeLight(n)` create or delete a Light <N> group of the five light knobs, with `light<N>Position->setHasHostOverlayHandle(true)` as `Position.cpp:55` does; buttons call them as one undoable edit. A hidden `lightCount` knob drives recreation on load and is the Python-visible count. Removing a middle light renumbers nothing: groups keep their numbers.
  - verify: gtest: add two lights, animate one position, save and reload the project: three groups with the animation intact; remove light 2: lights 1 and 3 remain; undo restores light 2.
  - size: L

- [ ] M466.P1.T3 — Coating knobs and table
  - files: `Engine/Nodes/Generator/LensFlare.{h,cpp}`, `Tests/Native/NativeLensFlare_Test.cpp`
  - approach: On a lens change, seed change or Randomize, fill the `coatings` KnobTable (one row per coatable surface: Surface, Wavelength, IOR) from M462's `generateCoatings(prescription, seed, ranges)`. User edits to the table are kept until the lens, seed or ranges change. The render reads the table.
  - verify: gtest: the same seed gives the same table; Randomize changes the seed and the table; an edited IOR survives a save and reload and reaches the trace setup.
  - size: M

## Phase 466.2: Ghost engine (CPU)

- [ ] M466.P2.T1 — Light directions, ray grid and preprocessing
  - files: `Engine/Nodes/Generator/LensFlareGhosts.{h,cpp}` (new, engine-free apart from `Engine/Optics`), `Engine/CMakeLists.txt`, `Tests/Native/LensFlareGhosts_Test.cpp` (new)
  - approach: Light direction from its position: pixel → normalised film coordinates over `sensorSize` (vertical extent following the format's aspect, as in realflare) → angle through the lens's paraxial EFL (M462). Port `DiscMesh` (hexagonal grid, 1+3d(d+1) vertices, 6d² triangles, neighbour lists). Enumerate ghosts with M462. Preprocess as `preprocess.py`: trace every ghost at 16 divisions and one wavelength, per-triangle area and intensity; drop ghosts with mean intensity ≤ 1e-9 and cull the lowest `cullPercentage` by mean intensity; map divisions from `minDivisions` to `maxDivisions` by log1p of the triangle-area CV²; fit each ghost's grid centre and radius to its valid rays (finite reflectance, rrel ≤ 1, stop position ≤ 1/f-stop) with one division of padding, rotated to the light direction.
  - verify: gtest: an on-axis light gives grids centred on the axis; `cullPercentage` 0.5 keeps half of the non-zero ghosts; a higher-distortion ghost gets more divisions; the grid of a ghost covers all its valid rays.
  - size: L

- [ ] M466.P2.T2 — Full trace and triangle weights
  - files: `Engine/Nodes/Generator/LensFlareGhosts.{h,cpp}`, `Tests/Native/LensFlareGhosts_Test.cpp`
  - approach: Trace each surviving ghost's grid at `wavelengths` mid-bin wavelengths (M462). Per triangle and wavelength, as `flare_prim.comp`: shoelace area, intensity = original area / max(area, minArea × original area), sliver factor 4√3·A/(a²+b²+c²) through smoothstep(clamp(minSliver − 0.5, 0, 0.1), minSliver, q), and culling when every ray is invalid or outside the housing. Per-vertex intensity averages the valid neighbouring triangles. Multi-threaded per ghost.
  - verify: gtest: an undistorted ghost's triangles have intensity ≈ 1; a collapsing triangle is floored by minArea; minSliver 1 removes slivers; a fully vignetted ghost produces no triangles.
  - size: M

- [ ] M466.P2.T3 — CPU ghost rasteriser
  - files: `Engine/Nodes/Generator/LensFlareRaster.{h,cpp}` (new), `Tests/Native/LensFlareGhosts_Test.cpp`
  - approach: Rasterise every ghost triangle additively into a float RGB buffer: edge functions with the top-left rule and pixel-centre sampling (the rules M472 - GPU Raster Pipelines' hardware raster uses), interpolating per-vertex intensity, reflectance and the stop-position UV. Fragment value as `flare.frag`: smoothstep(1, 0.95, rrel) × ghost image(UV) × intensity × reflectance / total wavelengths × the wavelength's working-space colour (M462 `Spectral`) × the light's illuminant and tint. Sub-wavelengths blend traced wavelength k and k+1. `supersampling` 2x rasterises at double size and box-downsamples. `wireframe` draws triangle edges; `isolateGhost` keeps one ghost. Multi-threaded by row bands.
  - verify: gtest: a unit-intensity triangle covering known pixels sums to its area within 1%; two triangles sharing an edge never double-cover it; 2x supersampling preserves total energy within 0.5%; isolate keeps exactly one ghost's energy.
  - size: L

- [ ] M466.P2.T4 — Ghost aperture image
  - files: `Engine/Nodes/Generator/LensFlareGhosts.{h,cpp}`, `Tests/Native/LensFlareGhosts_Test.cpp`
  - approach: Port `ghost.py`: render the `ghostAperture` mask (M465) at `ghostResolution`, with the "Ghost Aperture" input as its image layer, then Fresnel propagation by the angular spectrum method at 560 nm over a 0.1 window with M464's CPU FFT: ifft2(fft2(A)·H), H = exp(i z √(k² − kx² − ky²)) with evanescent waves clamped, |·|², normalised to its maximum, × a circular fade from `ghostVignette`. Cached by its parameters.
  - verify: gtest: distance 0 returns the aperture itself within 1e-4; a larger distance adds ringing at the edge (energy outside the aperture rises); identical parameters hit the cache.
  - size: M

## Phase 466.3: Render and finish

- [ ] M466.P3.T1 — Render action, caching and cancel
  - files: `Engine/Nodes/Generator/LensFlare.{h,cpp}`, `Tests/Native/NativeLensFlare_Test.cpp`
  - approach: For each enabled light, run P2.T1–T3 and sum. Cache the trace (P2.T1–T2 results) per light, keyed by the resolved prescription hash, glass vendor, Abbe offset, `useAspheric`, coatings table, `sensorSize`, `fStop`, raytracing knobs and the light direction, LRU-bounded; the raster runs per render. Abort checks between ghosts and bands. Add the content hash of the knobs and the Ghost Aperture input. Alpha stays 0.
  - verify: gtest: a Nikon 28-85 default render is deterministic across two runs; moving a light re-traces only that light; changing intensity re-rasterises without re-tracing (counter); an abort mid-render returns within 1 s.
  - size: M

- [ ] M466.P3.T2 — Control audit against realflare
  - files: `docs/lensflare/controls-audit.md` (new)
  - approach: Table every control in realflare's `flare/api/project/model.py` (Flare tab: light, lens, camera, raytracing, render, debug; Ghost tab; the aperture form): realflare name, our knob, default, milestone. Starburst and output rows name M468 - LensFlare Starburst and M469 - LensFlare Layers And Diagram. No row may say MISSING; ruled-out controls name the decision.
  - verify: every field of realflare's `Project` model appears in the table, with no MISSING rows.
  - size: S

- [ ] M466.P3.T3 — Goldens, docs and GUI check
  - files: `Tests/Native/goldens/LensFlare/` (new, ≤256 px half-float crops), `Engine/Nodes/Generator/docs/LensFlare.md`, `Tests/gui/m466_lensflare.py` (new)
  - approach: Goldens of the default lens and light, an off-axis light, coatings seed 1 and an isolated ghost. Docs: lights, lens selection, coatings, quality knobs and cost, and the sources (realflare, Hullin 2011, Ritschel 2009). The GUI script adds a second light, drags both handles under Xvfb and screenshots the viewer.
  - verify: `ctest -R "NativeLensFlare|LensFlareGhosts"` passes; the GUI script passes; screenshots are shared with the user (collected at F31 - Lens Flares' UAT).
  - size: M

**Verification gate:** build and ctest green; the control audit has no MISSING rows; GUI check screenshots shared; a default 2K CPU render time is recorded in Decisions.

## Decisions

- 2026-11-16 — **Pure generator, alpha 0 (user, /cat-plan grill):** no Source input; the flare is emitted light, comped downstream.
- 2026-11-16 — **Several lights per node as dynamic groups of real knobs (user):** `KnobTable` cells can't animate or carry viewer handles (Engine/KnobTypes.h:1600), so each light is a numbered group of animatable knobs with its own handle.
- 2026-11-16 — **Full CPU path with GPU parity (user):** the CPU tracer and rasteriser serve GPU-less renders and are the parity reference.
- 2026-11-16 — **Derive sizes physically, match realflare's look (user):** realflare's fudge factors are not ported.
