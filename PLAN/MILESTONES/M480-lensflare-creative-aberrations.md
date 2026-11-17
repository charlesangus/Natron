# M480 - LensFlare Creative Aberrations

Give LensFlare the creative pupil terms from M474 - Traced Lens Creative Aberrations (user, 2026-11-16: "Wire into LensFlare too"). Ghosts and the starburst are traced through the same `Engine/Optics` tracer, so the same `PupilTerms` reshape them: coma-tailed ghosts, astigmatic stretch, ripple rings and lobed ghost shapes. One shared knob group gives both nodes the same controls and looks. It ships alone, not in a Feature, once F13 - ZDefocus Rework and F31 - Lens Flares have both landed, so neither Feature waits on the other.

Deps: M474 - Traced Lens Creative Aberrations, M466 - LensFlare Ghost Tracing, M467 - LensFlare GPU Ghosts, M468 - LensFlare Starburst.

## Scope

**In:**
- M474's `LensCreativeKnobs` on LensFlare (a "Creative" group), with the same script names, defaults and Look menu.
- `PupilTerms` in LensFlare's CPU ghost trace and its GPU ghost shader (M467), and in the starburst's pupil function (M468) where a term applies.
- Goldens, docs and a GUI check.

**Out:**
- Any change to bokeh: M474 owns it.
- Looks specific to flares: the shared Look menu only, unless the user asks later.

**Edge cases:**
- Every term at 0: bit-identical to LensFlare before this milestone.
- Terms that only have meaning off axis (coma, astigmatism, lobes face-centre) use the light's field position.
- Axial colour terms apply per traced wavelength, using the wavelength's band.

**Assumptions:**
- Unconfirmed: ghost shapes respond visibly at the same mm magnitudes as bokeh. If not, the docs give flare-specific suggested ranges, and no knob changes.
- Unconfirmed: the starburst (a far-field diffraction of the aperture) takes only the Pupil Phase-like terms; the geometric terms leave it unchanged. Record which terms apply.

## Phase 480.1: Ghosts and starburst

- [ ] M480.P1.T1 — Pupil terms in the CPU ghost trace and knobs on LensFlare
  - files: `Engine/Nodes/Filter/LensFlare.{h,cpp}` (or wherever M466 put the node), `Engine/Nodes/Filter/LensCreativeKnobs.{h,cpp}`, `Tests/Native/NativeLensFlare_Test.cpp` (M466's test file)
  - approach: Add the Creative group through `LensCreativeKnobs`. Pass `PupilTerms` and the light's field position into `traceGhost`, so ghost rays land on the shifted sensor plane as bokeh rays do.
  - verify: tests: all terms at 0 are bit-identical to the existing LensFlare goldens; coma 0.05 shifts an off-axis ghost's centroid along the field direction and flips with sign; lobes 3 gives a ghost with 3-fold symmetry.
  - size: L

- [ ] M480.P1.T2 — Pupil terms in the GPU ghosts and the starburst
  - files: `Engine/Gpu/Kernels/` (M467's ghost shader, which includes `opticsTrace.slang`), the starburst source from M468, `Tests/Native/NativeLensFlareGpu_Test.cpp` (M467's)
  - approach: Pass the terms to the GPU ghost pass through the shared include's perturbation. Add the applicable terms to the starburst's pupil function and record which apply in `## Decisions`.
  - verify: GPU parity with the CPU for each term at a non-zero value, within LensFlare's tolerance, on RADV at the phase build.
  - size: M

## Phase 480.2: Docs and GUI

- [ ] M480.P2.T1 — Goldens, docs and GUI check
  - files: `Tests/Native/goldens/LensFlare/` (creative goldens), LensFlare's docs page, `Tests/gui/m480_flare_creative.py` (new)
  - approach: Goldens for coma, astigmatism and lobes on an off-axis light as ≤256 px half-float crops. Docs list the shared terms and how they reshape ghosts. Never name other products. The GUI script screenshots each look on a flare.
  - verify: `ctest -R LensFlare` passes; the GUI script passes, and its screenshots are shared with the user for UAT.
  - size: M

**Verification gate:** the full build and `ctest` pass, with GPU parity on RADV; all terms at 0 are bit-identical to LensFlare's goldens; the GUI screenshots are shared, and an AppImage built with `package.sh` is handed over for UAT.

## Decisions

- 2026-11-16 — **Wire the creative terms into LensFlare (user, /cat-plan grill).**
- 2026-11-16 — **A standalone milestone after both Features land (planner):** keeps F13 - ZDefocus Rework and F31 - Lens Flares independent.
