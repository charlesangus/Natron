# M474 - Traced Lens Creative Aberrations

Creative aberrations on top of a traced lens, built as physical pupil terms so each one is tied to focus, changes sign across it and scales with the bokeh (Higx Optics parity, user 2026-11-16). The tracer lands every ray on a sensor plane shifted by Δz(pupil position, field), in mm of sensor travel. Field curvature, astigmatism, coma, spherical-style "impression", zonal ripple, lobes and axial colour are all terms of that one function. The terms live in `Engine/Optics`, so M480 - LensFlare Creative Aberrations can reuse them. ZDefocus and Defocus get the knobs and a Look menu of named presets.

Deps: M292 - Traced Lens Bokeh (`LensTable`, the GPU bake, `LensTableCache`).

## Scope

**In:**
- `Optics::PupilTerms` and the sensor-plane perturbation in the CPU tracer (`Engine/Optics/Trace`) and the GPU include (`opticsTrace.slang`). Pupil coordinates are normalised to the stop; field coordinates come from the table's ρ (and from x/y with M476 - Traced Anamorphic Bokeh).
- Knobs on ZDefocus and Defocus, shared through one helper, enabled only with a traced lens. Labels follow the script names:

  | Knob | Default | Term (r = pupil radius, φ = pupil angle from the field direction, h = field radius) |
  |---|---|---|
  | `impression` | 0 | Δz = a·r^(2p): one sign gives creamy rims, the other bright, harsh rims; mm |
  | `impressionPower` | 1 | p; range 1–8; low reshapes the whole disc, high only the rim |
  | `coma` | 0 | Δz = c·h·r·cos φ; the sign sets the tail direction; mm at the corner |
  | `astigmatism` | 0 | Δz = s·h²·cos²φ; the tangential/sagittal focus split at the corner; mm |
  | `fieldCurvature` | 0 | Δz = k·h²; one sign gives soft corners, the other a soft centre; mm |
  | `zonalRipple` | 0 | Δz = z·sin(2π·n·r²); onion rings that flip across focus; mm |
  | `rippleRings` | 3 | n; range 1–12 |
  | `lobes` | 0 | Δz = l·r^q·cos(m(φ − θ)); rounded m-cornered discs that flip through focus; mm at the iris edge |
  | `lobeCount` | 3 | m; range 3–8 |
  | `lobePower` | 3 | q; range 1–8 |
  | `lobeAngle` | 0 | θ; degrees |
  | `lobesFaceCenter` | off | rotates θ per position so one lobe points at the frame centre, the amount fading from 0 at the centre to full at the corner |
  | `caGreenMagenta` | 0 | green's focus offset from the red/blue pair; mm |
  | `caRedBlue` | 0 | red against blue; mm |
  | `flipInternals` | off | rotates the kernel 180°, flipping swirl and cat-eye asymmetry |
  | `creativeLook` | Custom | choice of named looks that set the knobs above |

- Looks (user, 2026-11-16: a Look knob rather than node presets, since Natron's presets are Load/Save of `.nps` files only, `Gui/NodeSettingsPanel.cpp:230`): Custom, Swirl Edges (a clean centre with stretched, turned highlights at the edges: coma and astigmatism), Soap Bubble (bright rims), Creamy (soft rims), Trefoil (3 lobes), Onion Rings (zonal ripple), Vintage Fringe (axial colour). Our own names.
- CPU and GPU (in M292's GPU bake), goldens, docs and a GUI check.

**Out:**
- LensFlare: M480 - LensFlare Creative Aberrations wires the same terms into ghosts and starburst (user, 2026-11-16).
- Procedural lenses: they keep M133 - ZDefocus Lens Aberrations's knobs; these knobs are disabled for Procedural.
- Squeeze: the existing `aspectRatio`, and M476 - Traced Anamorphic Bokeh for real anamorphics.

**Edge cases:**
- Every term at 0: traces bit-identical to M292, with the same cache key.
- In focus: a term's residual blur at the focal plane is physical, and its size is the term's Δz times the ray slope. A few hundredths of a mm stays under half a pixel at 2K; docs say so.
- CA with lensColour off: any non-zero CA term forces per-channel tables (R/G/B at 610/550/465 nm), since the offsets are per channel.
- Choosing a look writes its knob values as one undoable edit. Editing any creative knob afterwards sets `creativeLook` back to Custom without changing values.
- Animated terms: each frame's terms are part of the cache key, and the GPU bake keeps it interactive.

**Assumptions:**
- Unconfirmed: slider ranges are ±0.1 mm for the mm terms, typed values beyond them are accepted, and "a few hundredths" is the useful range, as with the reference product.
- Unconfirmed: landing each ray on a shifted plane is close enough to a true wavefront perturbation for creative use at these magnitudes, with no diffraction.

## Phase 474.1: Pupil terms

- [ ] M474.P1.T1 — Pupil terms in the CPU tracer
  - files: `Engine/Optics/PupilTerms.{h,cpp}` (new), `Engine/Optics/Trace.{h,cpp}`, `Tests/Native/OpticsPupilTerms_Test.cpp` (new)
  - approach: `PupilTerms` holds the Scope's term amounts. `deltaZ(r, φ, hx, hy, channel)` evaluates their sum. `traceSequential` takes an optional `PupilTerms` plus field coordinates, records the ray's normalised stop position (it already tracks it), and intersects the image plane at z_image + Δz in place of z_image. Per-channel CA adds its offset by wavelength band. No Natron or Qt types.
  - verify: gtest: zero terms are bit-identical to unperturbed traces; field curvature 0.05 mm moves the best focus at h = 1 by 0.05 mm within 1%; coma's spot centroid shift grows linearly with h and flips with sign; astigmatism 0.05 splits the tangential and sagittal foci by 0.05 mm within 2%; lobes m = 3 gives a spot with 3-fold symmetry.
  - size: L

- [ ] M474.P1.T2 — Pupil terms in the bokeh table and GPU bake
  - files: `Engine/Nodes/Filter/LensTable.{h,cpp}`, `Engine/Nodes/Filter/LensTableCache.{h,cpp}`, `Engine/Gpu/Kernels/opticsTrace.slang`, `Engine/Gpu/Kernels/lenstable.slang`, `Engine/Gpu/GpuLensTable.{h,cpp}`, `Tests/Native/LensTable_Test.cpp`, `Tests/Native/LensTableGpu_Test.cpp`
  - approach: Pass `PupilTerms` into the CPU and GPU bakes; add them to the cache key. The table's per-(u, ρ, channel) kernel fields capture the perturbed footprints, with no new kernel field types. If zonal ripple's rings are finer than the 8-ring profile can hold, raise the profile's ring count when `zonalRipple` ≠ 0, and record the count chosen.
  - verify: tests: zero terms give the M292 table exactly; zonal ripple's ring profile has opposite phase in front of and behind focus; CPU and GPU tables agree per M292's `LensTableGpu_Test` bounds for each term at a non-zero value.
  - size: L

## Phase 474.2: Knobs and looks

- [ ] M474.P2.T1 — Creative knobs on ZDefocus and Defocus
  - files: `Engine/Nodes/Filter/LensCreativeKnobs.{h,cpp}` (new), `Engine/Nodes/Filter/ZDefocus.{h,cpp}`, `Engine/Nodes/Filter/Defocus.{h,cpp}`, `Engine/Nodes/Filter/BokehKernel.{h,cpp}` (flipInternals), `Tests/Native/NativeZDefocus_Test.cpp`, `Tests/Native/NativeDefocus_Test.cpp`
  - approach: A helper like `BokehKnobs` adds the Scope's knobs in a "Creative" group on the Bokeh tab, enabled only with a traced lens, and reads them into `PupilTerms`. `lobesFaceCenter` resolves θ per cell from the cell's canonical position. `flipInternals` rotates the rasterised kernel 180° in `BokehKernel::fromTraced`. Per cell, the nodes pass the terms through `LensTableCache`.
  - verify: tests: all terms at 0 are bit-identical to M292's goldens; CA green/magenta 0.05 gives a green-dominant rim behind focus and a magenta one in front; lobes 3 with face-centre on rotates between two corner cells by their angle difference; flipInternals mirrors an off-axis Petzval kernel through its centre.
  - size: L

- [ ] M474.P2.T2 — Look menu
  - files: `Engine/Nodes/Filter/LensCreativeKnobs.{h,cpp}`, `Tests/Native/NativeZDefocus_Test.cpp`
  - approach: `creativeLook` (choice) at the top of the Creative group. Each look is a table of knob values, documented in the header comment. On a user change, set the values in one undo group. A change to any creative knob that isn't from the look sets the choice back to Custom (guard with a flag while the look writes).
  - verify: tests: each look sets its table's values; editing `coma` afterwards shows Custom and keeps the other values; undo restores the previous values in one step.
  - size: M

## Phase 474.3: Docs and GUI

- [ ] M474.P3.T1 — Goldens, docs and GUI check
  - files: `Tests/Native/goldens/ZDefocus/` (creative goldens), `Engine/Nodes/Filter/docs/ZDefocus.md`, `Engine/Nodes/Filter/docs/Defocus.md`, `Tests/gui/m474_creative.py` (new)
  - approach: Goldens on a point-highlight grid with a depth ramp through Double-Gauss: impression ±, coma, astigmatism, zonal ripple and lobes, as ≤256 px half-float crops. The docs explain each term in mm of sensor travel, the in-focus residual and each look. Never name other products. The GUI script under Xvfb renders each look, plus each term in front of and behind focus, and screenshots each as its own file.
  - verify: `ctest -R "NativeZDefocus|NativeDefocus|Optics"` passes; the GUI script passes, and its screenshots are collected for F13 - ZDefocus Rework's UAT.
  - size: M

**Verification gate:** the full build and `ctest` pass, with GPU parity on RADV; all terms at 0 are bit-identical to M292 - Traced Lens Bokeh; M292's focus-rack timing still meets ≤ 30 ms per bake with a non-zero term set (rerun `tools/bench/zdefocus_focus_rack.py` with the Swirl Edges look); the GUI screenshots are collected at F13 - ZDefocus Rework's UAT.

## Decisions

- 2026-11-16 — **Pupil terms, physically tied to focus (user, /cat-plan grill):** rather than re-enabling M133's procedural knobs on traced lenses.
- 2026-11-16 — **A Look knob (user):** the user first chose node presets, then a Look knob once told presets are only `.nps` Load/Save.
- 2026-11-16 — **Terms in the shared optics library, wired into LensFlare too (user):** the LensFlare wiring is M480 - LensFlare Creative Aberrations, which waits for F31 - Lens Flares, so neither Feature waits on the other.
