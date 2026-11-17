# M473 - Lens Camera And Focus Controls

Camera and focus controls for ZDefocus and Defocus, so traced lenses are set up the way a camera is (Higx Optics parity, user 2026-11-16). Adds a film format menu with generic and cinema sensor formats, a Filmback Offset focus method in mm of sensor travel, depth Gain/Bias, a Size Scale on the physical CoC, a Lens Coverage control for how far edge character reaches, and highlight gamma and rolloff. Builds on M292 - Traced Lens Bokeh's Lens knobs and `LensTable`.

Deps: M292 - Traced Lens Bokeh.

## Scope

**In:**
- `filmFormat` (label "Film Format") on ZDefocus (Lens mode) and Defocus's Lens tab. It sets `filmBackWidth` from a cited table and disables it unless Custom. Entries (user, 2026-11-16): Custom; Full Frame 3:2; Full Frame 16:9; Super 35 3-perf; Super 35 4-perf; Super 35 16:9; Super 16; APS-C 3:2; Micro Four Thirds; Medium Format 4:3; ARRI Alexa 35 Open Gate; ARRI Alexa Mini LF Open Gate; ARRI Alexa 65 Open Gate; RED V-Raptor 8K VV; Sony Venice 2 8.6K; IMAX 15/70. Default Full Frame 3:2, whose 36 mm is today's default width.
- `focusMethod` (Focal Plane, the default, or Filmback Offset) and `filmbackOffset` (mm of sensor travel; 0 is infinity, positive focuses closer, negative racks past infinity) on ZDefocus (Lens mode) and Defocus's Lens tab (user, 2026-11-16).
- `depthGain` (1) and `depthBias` (0) on ZDefocus: depth' = depth × gain + bias, applied before `depthUnits`. A negative gain flips near and far.
- `sizeScale` (label "Size Scale", 1, range 0–10) on ZDefocus Lens mode and on Defocus with `sizeFromLens` on: a multiplier on the physical CoC that leaves the kernel's character unchanged.
- `lensCoverage` (1, range 0–2) for traced lenses: scales the field radius ρ, so 0 gives every position the on-axis kernel and 2 pulls the corner look towards the centre.
- `highlightGamma` (1) and `highlightRolloff` (0 = off) on ZDefocus and Defocus, CPU and GPU.

**Out:**
- A unit-free normalised Focus slider: not wanted (user, 2026-11-16); Focal Plane and the Focal Point picker cover it.
- An Edge Field Angle knob: the corner field comes from the film back and the traced lens's focal length, and Custom plus `lensCoverage` cover overrides.
- A Source Distance knob: every pixel's character comes from its own depth through the table's u axis, which exceeds a single subject distance.
- Input encoding and log decoding: Natron works scene-linear and colour management is upstream.
- Creative aberrations: M474 - Traced Lens Creative Aberrations. Aperture maps and front filters: M475 - Aperture Maps And Front Filter.

**Edge cases:**
- Every new knob at its default: bit-identical to M292 - Traced Lens Bokeh's goldens.
- Negative Filmback Offset (past infinity): no depth is in focus, every depth is on the near side of the virtual focus, and the CoC sign and size follow the paraxial image equation.
- A Filmback Offset that focuses nearer than the lens's focal length: traced as is, with no clamp (no hand-holding caps).
- `lensCoverage` > 1 extends the table's field range to ρ = coverage. A field point beyond the lens's image circle gets the kernel of the last field radius that transmits light, and Absolute vignetting darkens it.
- Procedural lenses: Filmback Offset uses the thin lens; `lensCoverage` scales M133's cat-eye position the same way.
- Highlight rolloff: the boost's added amount tapers smoothly to 0 as a value approaches `highlightRolloff`. Values at or above it are unchanged, and the boost never darkens.

**Assumptions:**
- Unconfirmed: the film back width anchors the image width, as ZDefocus does today, so a format's height is informational (docs and M477 - Lens Cross-Section Panel's sensor drawing) and does not change the render.
- Unconfirmed: format dimensions come from the camera makers' published specifications, and each table row cites one.

## Phase 473.1: Film formats

- [ ] M473.P1.T1 — Film format table and knob
  - files: `Engine/Optics/FilmFormats.{h,cpp}` (new), `Engine/Nodes/Filter/ZDefocus.{h,cpp}`, `Engine/Nodes/Filter/Defocus.{h,cpp}`, `Tests/Native/NativeZDefocus_Test.cpp`, `Tests/Native/NativeDefocus_Test.cpp`
  - approach: `Optics::filmFormats()` returns the Scope's entries as (id, label, width mm, height mm, source URL), with values from each maker's published sensor specification and the URL in the table. Add `filmFormat` (choice, label "Film Format") beside `filmBackWidth` on both nodes. Changing it writes the width as one undoable edit, and `filmBackWidth` is enabled only for Custom. Labels follow script names.
  - verify: tests: each format sets the table's width; Custom enables the width knob; the default renders bit-identical to the existing goldens.
  - size: M

## Phase 473.2: Focus and depth

- [ ] M473.P2.T1 — Filmback Offset focus method
  - files: `Engine/Nodes/Filter/ZDefocus.{h,cpp}`, `Engine/Nodes/Filter/Defocus.{h,cpp}`, `Engine/Nodes/Filter/DepthOfField.h`, `Tests/Native/NativeZDefocus_Test.cpp`, `Tests/Native/NativeDefocus_Test.cpp`
  - approach: Add `focusMethod` (Focal Plane, Filmback Offset) and `filmbackOffset` (mm). With Filmback Offset the focus distance comes from the image equation: for the thin lens, S = f (f + δ) / δ for offset δ > 0, with infinity at 0 and past infinity for δ < 0. For a traced lens, invert M462's `Paraxial` focusing (back focus + δ). Disable `focalPlane` (or Defocus's `focusDistance`) while Filmback Offset is chosen. The Focal Point overlay is hidden in that mode.
  - verify: tests: offset 0 makes the far depth sharp; the thin-lens offset for 5 m at 50 mm renders identically (1e-5) to Focal Plane 5 m; with Double-Gauss, the offset `Paraxial` gives for 5 m focuses at 5 m within 1%.
  - size: M

- [ ] M473.P2.T2 — Depth Gain and Bias
  - files: `Engine/Nodes/Filter/ZDefocus.{h,cpp}`, `Engine/Gpu/Kernels/zdefocus.slang`, `Tests/Native/NativeZDefocus_Test.cpp`, `Tests/Native/NativeZDefocusGpu_Test.cpp`
  - approach: Add `depthGain` and `depthBias` next to `depth`, applied to the sampled depth before `depthUnits` on CPU and GPU, and in the Focal Point pick, so picking still focuses the picked pixel.
  - verify: tests: defaults are bit-identical; gain 2 with Distance units matches a render of a doubled depth channel exactly; gain −1, bias 10 flips near and far; GPU parity within the node's tolerance.
  - size: M

## Phase 473.3: Size, coverage and highlights

- [ ] M473.P3.T1 — Size Scale and Lens Coverage
  - files: `Engine/Nodes/Filter/ZDefocus.{h,cpp}`, `Engine/Nodes/Filter/Defocus.{h,cpp}`, `Engine/Nodes/Filter/LensTable.{h,cpp}`, `Engine/Nodes/Filter/LensTableCache.{h,cpp}`, `Engine/Nodes/Filter/LensAberration.{h,cpp}`, `Tests/Native/NativeZDefocus_Test.cpp`
  - approach: `sizeScale` multiplies the signed CoC after the Lens-mode computation and before `maxSize`. u stays from the unscaled geometry, so the shape is unchanged. `lensCoverage` multiplies ρ before the table lookup. The table bakes field radii up to max(1, coverage), which goes into the cache key. Procedural lenses scale M133's cat-eye position by it.
  - verify: tests: sizeScale 2 doubles every slice's diameter; coverage 0 gives every cell the ρ = 0 kernel; coverage 1 is bit-identical to M292; coverage 2 at ρ = 0.5 equals coverage 1 at ρ = 1 within 1e-6.
  - size: M

- [ ] M473.P3.T2 — Highlight gamma and rolloff
  - files: `Engine/Nodes/Filter/Defocus.{h,cpp}`, `Engine/Nodes/Filter/ZDefocus.{h,cpp}`, `Engine/Gpu/Kernels/defocus.slang`, `Engine/Gpu/Kernels/zdefocus.slang`, `Tests/Native/NativeDefocus_Test.cpp`, `Tests/Native/NativeDefocusGpu_Test.cpp`, `Tests/Native/NativeZDefocusGpu_Test.cpp`
  - approach: Today's boost multiplies the excess over `highlightThreshold` by `highlightGain`. Normalise the excess to the threshold, raise it to `highlightGamma`, then apply the gain. A gamma above 1 lifts the mids of the excess, and below 1 only the brightest. With `highlightRolloff` > 0, multiply the added amount by a smooth taper reaching 0 at the rolloff value (smoothstep over the last 20% below it), and leave values at or above it alone. Same maths in both shaders.
  - verify: tests: defaults are bit-identical; across 10⁴ random values the boosted value is never below the input; values ≥ rolloff are unchanged; gamma 2 boosts a value just above the threshold less than gamma 1 does; GPU parity.
  - size: M

## Phase 473.4: Docs and GUI

- [ ] M473.P4.T1 — Docs, goldens and GUI check
  - files: `Engine/Nodes/Filter/docs/ZDefocus.md`, `Engine/Nodes/Filter/docs/Defocus.md`, `Tests/gui/m473_camera_focus.py` (new)
  - approach: Document every new knob with its default and range, and the film format table with its sources. Never name other products. The GUI script under Xvfb picks Super 35 4-perf (the width knob is disabled), switches to Filmback Offset (Focal Plane disabled), sets Lens Coverage 0 and 2 on a Petzval render, and screenshots each.
  - verify: `ctest -R "NativeZDefocus|NativeDefocus"` passes; the GUI script passes, and its screenshots are collected for F13 - ZDefocus Rework's UAT.
  - size: M

**Verification gate:** the full build and `ctest` pass, with GPU parity on RADV; every new knob at its default is bit-identical to M292 - Traced Lens Bokeh's goldens; the GUI script passes, and its screenshots are collected at F13 - ZDefocus Rework's UAT.

## Decisions

- 2026-11-16 — **Generic and cinema film formats (user, /cat-plan grill):** the generic formats plus Alexa 35, Mini LF, Alexa 65, V-Raptor VV, Venice 2 and IMAX 15/70, all cited.
- 2026-11-16 — **Filmback Offset is the only new focus method (user):** a normalised focus slider is not wanted; Focal Plane and the picker cover everyday use.
- 2026-11-16 — **No close-focus clamp (planner, per the no-hand-holding rule):** a focus nearer than the lens allows is traced as is.
