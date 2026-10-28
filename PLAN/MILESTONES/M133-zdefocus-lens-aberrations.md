# M133 - ZDefocus Lens Aberrations

A follow-up to M131 - Native Defocus Family that makes ZDefocus and Defocus behave like real lenses:
- **Cat-eye (optical) vignetting.** The bokeh is clipped by a second aperture as a function of image position, so discs turn into cat-eyes towards the frame edges.
- **Chromatic aberration.** The CoC or kernel scale differs per channel, giving coloured bokeh fringes.
- **Anamorphic squeeze.** Oval bokeh with a squeeze ratio.
- **Possibly textured kernels.** Swirly bokeh and onion-ring kernel textures.

Each ships CPU and GPU, with parity tests, on M131 - Native Defocus Family's bokeh kernel library and ZDefocus gather.

Deps: M131 - Native Defocus Family.

## Scope

**In:** cat-eye vignetting plus a swirl variant (cat-eye turned tangential) on Defocus and ZDefocus; chromatic aberration as a per-channel CoC scale on both; anamorphic squeeze through the existing `aspectRatio` knob (`Defocus.h:47`, `ZDefocus.h:69`), honoured by the new aberrations; kernel textures in `BokehKernel` as in Nuke's ZDefocus/Bokeh (onion rings: inner size, feather, brightness; catadioptric hole; spherical aberration; Gaussian-to-disc filter shape); CPU, GPU and parity tests.

**Out:** deep input (E8); new IDefocus knobs (M186 inherits the BokehKernel changes only); a GPU path for Image kernels (M131 open question); lens flare and ghosting.

**Edge cases:**
- Every new knob at default → bit-identical to M131 (existing goldens, exact compare).
- Position = canonical position relative to the input format's centre / half-diagonal; never depends on the render window (tiles, RoI, proxy agree).
- Cat-eye kernels per cell of a fixed format-anchored grid (32 full-res px, scaled by render scale), using the cell centre.
- A clip that empties a kernel falls back to the unclipped one; Image kernels are clipped then renormalised.
- CA: alpha and slice coverage use the green scale; a channel whose scaled CoC exceeds maxSize clamps on its own. Highlight boost per channel before the gather (unchanged).

**Assumptions:** a 32 px grid looks continuous up to the 500 px max size (unconfirmed); M131 merges with the BokehKernel/ZDefocus API as of `e32094c99`.

## Phase 133.1: Kernel shaping

- [ ] M133.P1.T1 — Second-aperture clip in the bokeh kernels
  - files: Engine/Nodes/Filter/BokehKernel.{h,cpp}, Tests/Native/BokehKernel_Test.cpp
  - approach: `clipOffsetX/Y`, `clipRadius` (in radius units) in BokehShapeParams; shaped()/fromImage() multiply by the clip disc's coverage (edge-supersampled) and renormalise; empty → unclipped fallback.
  - verify: zero offset bit-equal to M131; +x offset gives a lens shape symmetric about x; area shrinks monotonically with offset.
  - size: M
- [ ] M133.P1.T2 — Kernel textures
  - files: Engine/Nodes/Filter/BokehKernel.{h,cpp}, Tests/Native/BokehKernel_Test.cpp
  - approach: inner ring (size, feather, brightness), catadioptric hole size, spherical aberration weight (negative brighter centre, positive brighter edge), Gaussian-to-disc filter shape; defaults leave M131's kernels unchanged.
  - verify: radial profile gtests; defaults bit-equal.
  - size: M

## Phase 133.2: Aberrations in Defocus and ZDefocus (CPU)

- [ ] M133.P2.T1 — Engine-free lens-aberration model
  - files: Engine/Nodes/Filter/LensAberration.{h,cpp} (new), Tests/Native/LensAberration_Test.cpp
  - approach: from format, render scale and knobs, return each cell's clip params (radial for cat-eye, tangential for swirl) and per-channel CoC scales 1 + amount·offset_c.
  - verify: centre cell unclipped; corner cells clip radially (tangentially for swirl); grid identical across render windows.
  - size: M
- [ ] M133.P2.T2 — Defocus knobs and CPU path
  - files: Engine/Nodes/Filter/Defocus.{h,cpp}, Tests/Native/NativeDefocus_Test.cpp
  - approach: knobs catEye, catEyeOrientation (radial/tangential), chromaticAberration, chromaticOffset (RGB), plus the P1.T2 texture knobs; window split by cell, per-cell runs, kernels cached per cell and channel.
  - verify: centre point light = disc, corner = cat-eye; CA per-channel widths; aspectRatio 0.5 = 1:2 oval; defaults equal M131 goldens.
  - size: L
- [ ] M133.P2.T3 — ZDefocus knobs and CPU path
  - files: Engine/Nodes/Filter/ZDefocus.{h,cpp}, Tests/Native/NativeZDefocus_Test.cpp
  - approach: same knobs; kernels per slice × cell × channel; with CA, coverage per channel; Lens/Manual modes unchanged.
  - verify: same cases across two depth slices; no halo on a sharp foreground; defaults equal M131 goldens.
  - size: L

## Phase 133.3: GPU and verification

- [ ] M133.P3.T1 — Defocus GPU with per-cell run tables
  - files: Engine/Gpu/Kernels/defocus.slang, Engine/Nodes/Filter/Defocus.cpp, Tests/Native/NativeDefocusGpu_Test.cpp
  - approach: cell/channel run offsets in a storage buffer (push constants cap at 128 B); kernels ≤256 px stay on the GPU.
  - verify: parity 1e-4 abs / 1e-3 rel on RADV and lavapipe for each P2.T2 case.
  - size: L
- [ ] M133.P3.T2 — ZDefocus GPU with per-cell, per-channel runs
  - files: Engine/Gpu/Kernels/zdefocus.slang, Engine/Nodes/Filter/ZDefocus.cpp, Tests/Native/NativeZDefocusGpu_Test.cpp
  - approach: metaBuf gains per-cell/channel run offsets; coverage per channel with CA.
  - verify: parity for each P2.T3 case.
  - size: L
- [ ] M133.P3.T3 — Goldens and GUI script
  - files: Tests/fixtures/native-golden/{Defocus,ZDefocus}/, Tests/gui/m133_lens_aberrations.py
  - approach: synthetic point-light grid with a depth ramp; crops ≤256 px half-float; GUI renders centre/edge, swirl, CA, aspect 0.5 with screenshots.
  - verify: goldens recorded and re-run clean; remote GUI job passes.
  - size: M

**Verification gate:** full build + ctest; GPU parity on RADV; with new knobs at defaults M131's goldens unchanged; GUI check passes, screenshots shared; AppImage for UAT.

## Open questions (for the user; none blocks the start)
- Q1 Anamorphic squeeze via the existing aspectRatio knob (recommended) or a new Lens-mode anamorphicSqueeze that also squeezes the film back?
- Q2 All of Nuke's kernel texture controls (recommended, a port means every control) or only swirl and onion rings?
- Q3 A fixed 32 px cat-eye grid (recommended) instead of per-pixel kernels (O(r²) per pixel at 500 px)?

## Decisions

- 2026-10-28: Phases 1–2 implemented (build pending). Clip as a BokehClip struct (infinite default; a clip containing the shape is skipped → bit-identical). Texture defaults neutral (inner brightness 1, filter shape 1, spherical 0). Textured kernels quantised to 32 levels (±1/64 of peak before renormalising) to bound run counts (~66 runs/row at r=100); GPU tolerance can't bound this — the GPU gathers the same quantised kernels. Cell grid anchored at the format centre; cat-eye = second aperture of the kernel's radius offset by amount × position (radial) or a quarter turn (tangential/swirl); bands of ≥32 cell rows + 64 MB run cache. Coverage per channel group, alpha and non-colour channels on green (open: milestone text contradicts itself). Lens path is CPU-only until phase 3.
- 2026-10-28: elaborated (consultant); can start before Q1–Q3. Risk: the cell grid multiplies kernel builds (slices × cells × channels) — check on modest hardware.
- 2026-10-13 — **Created as a follow-up (user, /cat-plan grill):** an improved ZDefocus with aberrations, kept out of M131 - Native Defocus Family's v1.
