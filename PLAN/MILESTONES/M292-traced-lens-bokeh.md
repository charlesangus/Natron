# M292 - Traced Lens Bokeh

Real camera lenses for ZDefocus and Defocus, ported from Niik-l's LensTrace (remote `niikl`, `niikl/RB-2.6:Engine/Dev/PostFX/LensTrace.{h,cpp}`, GPL2+, pure C++17). The user calls the lens tracing "the most interesting part" of Niik-l's defocus work (2026-11-01). LensTrace traces a spherical-surface prescription sequentially and bakes a table. For each signed relative defocus u, normalised field radius ρ and colour channel, the table gives a kernel: a 2×2 map (real blur size, astigmatic stretch), up to 4 rim clip discs (mechanical cat-eye), a coma shift and an 8-ring radial profile (spherical aberration). Field maps add distortion, breathing, lateral colour and illumination. This milestone rasterises those kernels into our `BokehKernel` and uses M133 - ZDefocus Lens Aberrations's format-anchored cell grid for per-position variation. It also adds the four image stages.

Since 2026-11-16 (user, Higx Optics parity grill) the traced defocus must match or exceed Higx Optics (`https://higx.net/optics/`); M479 - Lens Bokeh Parity Audit holds the control inventory. This milestone adds a GPU table bake so racking focus stays interactive, physical vignetting modes, a cat-eye clip softness and two-axis breathing. The rest of the parity work is in M473 - Lens Camera And Focus Controls, M474 - Traced Lens Creative Aberrations, M475 - Aperture Maps And Front Filter, M476 - Traced Anamorphic Bokeh, M477 - Lens Cross-Section Panel and M478 - Cinema And Vintage Lens Prescriptions.

Since 2026-11-16 (user, lens flare planning) it builds on the shared lens base, F30 - Lens Optics Base: prescriptions, the `.zmx`/`.dat` parsers, catalogue Sellmeier dispersion, the sequential tracer and Niik-l's 9 built-in lenses come from M462 - Shared Optics Library; lens selection uses M463 - Bundled Lens Database's browser and knobs; the iris is drawn by M465 - Aperture Model. This milestone keeps LensTrace's bokeh-table baking, the kernels, the nodes' wiring and the image stages.

Deps: M131 - Native Defocus Family, M133 - ZDefocus Lens Aberrations (`BokehKernel`, `BokehClip`, the cell grid and per-channel coverage), M462 - Shared Optics Library, M463 - Bundled Lens Database, M465 - Aperture Model.

## Scope

**In:**
- LensTrace's bokeh-table baking (`LensTable::build/kernel/lookup/tracePoint`, field maps), ported engine-free onto M462 - Shared Optics Library's prescriptions and tracer. Every database lens works, including the 9 built-ins (Double-Gauss, 22mm wide, 250mm tele, fisheye, Cooke triplet, Tessar, Sonnar, Petzval, Planar) in its Classic Designs group, plus `.zmx` and pbrt/Kolb `.dat` files.
- The iris drawn by M465 - Aperture Model, so grating, scratches, dust and an aperture image shape the bokeh; with those at 0 the kernel is bit-identical to today's polygon iris.
- A live bokeh preview of the selected lens in M463 - Bundled Lens Database's browser.
- A Lens menu on ZDefocus (Lens and Manual CoC modes) and on Defocus. Defocus also gets a Focus Side choice.
- A **Lens tab on Defocus** (user, 2026-11-01). `size` stays the primary control. The tab holds `focalLength`, `fStop`, `filmBackWidth` (ZDefocus's script name), `focusDistance` and `objectDistance`, plus a `sizeFromLens` toggle, default off. When it is on, the blur size and focus side are computed from those values with ZDefocus's Lens-mode CoC model (`DepthOfField.h`), and `size` is disabled. The same knobs feed a traced lens's table, whether or not the toggle is on.
- Every Niik-l lens control:

  | Knob | Default | Notes |
  |---|---|---|
  | `lensModel` | Procedural | Choice: Procedural, Database, From File (M463's shared lens knobs) |
  | `lens` | Double-Gauss (Classic Designs) | Stable database lens id, with a Browse… button opening the lens browser |
  | `lensFile` | | `.zmx` or `.dat` |
  | `glassVendor` | Schott | Catalogue used to match each surface's glass (M462) |
  | `abbeOffset` | 0 | Range ±20 |
  | `useAspheric` | on | Off traces aspheres as spheres |
  | `lensCharacter` | 1 | |
  | `lensColour` | off | |
  | `clampToLens` | off | |
  | `lensDistortion` | 0 | Range 0–1 |
  | `lensBreathing` | (0, 0) | 2D (x, y), range −1 to 2; 1 is the real lens, negative inverts |
  | `lensLateralCA` | 0 | Range 0–1 |
  | `lensVignetting` | 0 | Range 0–1 |
  | `lensVignettingSpread` | 0 | Range 0–4 |
  | `lensVignettingMode` | Relative | Choice: Relative (physical falloff, frame centre held at source exposure), Absolute (full physical loss, including iris masking and stop-down) |
  | `lensClipSoftness` | 0 | Range 0–1; softens the cat-eye clip edge as a fraction of the kernel radius |

  Labels follow the script names: "Lens Model", "Lens", "Lens File", "Glass Vendor", "Abbe Offset", "Use Aspheric", "Lens Character", "Lens Colour", "Clamp To Lens", "Lens Distortion", "Lens Breathing", "Lens Lateral CA", "Lens Vignetting", "Lens Vignetting Spread", "Lens Vignetting Mode", "Lens Clip Softness".
- CPU and GPU, with parity tests, goldens, docs and a GUI check.
- A GPU table bake. Dragging focus on a 2K ZDefocus through a 7-element spherical lens with lensColour on bakes in ≤ 30 ms and updates at ≥ 10 fps on the GPU host, with a CPU fallback whose timing is recorded (user, 2026-11-16).
- Field variation without visible cell seams.

**Out:**
- IDefocus Lens knobs: M186 - Exact IDefocus inherits the `BokehKernel` changes only.
- Niik-l's LensDefocus node and its scatter engine: M294 - ZDefocus Engine Revisit decides what to borrow after M291 - Defocus Reference Harness.
- Tilt focus plane, World Point focus, a camera focus link, P/N passes, extra depth conventions and CleanBg: not requested (user, 2026-11-01).
- Traced lenses for CyclesRender, or any 3D renderer: no milestone.
- Film format presets, Filmback Offset focus, depth Gain/Bias, Size Scale, Lens Coverage and highlight gamma/rolloff: M473 - Lens Camera And Focus Controls.
- Creative aberrations on a traced lens: M474 - Traced Lens Creative Aberrations.
- RGB aperture maps, the Aperture input, iris size and front filters: M475 - Aperture Maps And Front Filter.
- Lenses with cylinder or biconic surfaces (anamorphic): M476 - Traced Anamorphic Bokeh.
- The live cross-section in the node panel: M477 - Lens Cross-Section Panel.
- Extra layer inputs at their own depth, glitter and a rain-on-glass distortion layer: not wanted (user, 2026-11-16), no milestone.

**Edge cases:**
- **Lens Model = Procedural:** bit-identical to M133 (existing goldens, exact compare).
- **Traced lens selected:**
  - The lens supplies outline, cat-eye, coma, spherical aberration and axial colour.
  - M133's procedural catEye, swirl, chromatic aberration and spherical-aberration knobs are disabled.
  - Blades and rotation still shape the iris. Onion rings, catadioptric hole, filter shape and highlight gain stack on top.
  - Lens Character 0 gives an ideal lens with the same iris (user, 2026-11-01).
- **Lens file that fails to parse or focus:** a node error naming the line, output passed through. Never a crash.
- **F-stop wider than the design allows:** `clampToLens` off keeps the blur size from F-Stop, and only the shape uses the widest stop. On, the blur size uses the widest stop too.
- **lensColour off:** a single green trace used for all channels. On: R/G/B traced at 610/550/465 nm, with coverage per channel through M133's per-channel path.
- **Image stages at 0:** no resampling at all, so the output is bit-identical with the stage off.
- **Distortion pulls in image from outside the frame:** samples outside the input's RoD follow the node's border handling.
- **Render scale and proxy:** ρ comes from the format-anchored canonical position, so kernels never depend on the render window.
- **Prescription with cylinder surfaces:** a node error saying the lens is anamorphic, with the output passed through, until M476 - Traced Anamorphic Bokeh adds 2D field tables.
- **Kernel Type = Image:** uses the Kernel image as the exact bokeh, with no tracing, as today. The lens knobs are disabled (user, 2026-11-16).
- **Animated focus or f-stop:** each frame looks up or bakes its own table. The GPU bake keeps a focus drag interactive, and the cache keeps scrubbing back cheap.
- **No GPU device:** the CPU bake runs. Results match the GPU bake within the parity tolerance.

**Assumptions:**
- Unconfirmed: in Manual CoC mode a traced lens builds its table from the Lens-mode knobs (focal length, f-stop, film back, focus distance), shown while a traced lens is selected. The CoC size still comes from the Manual knobs; only the shape and relative u come from the table.
- **Defocus with `sizeFromLens` on:** the CoC is computed for `objectDistance` against `focusDistance`, in pixels via film back ÷ format width and scaled by render scale. The sign sets the focus side and u comes from the lens geometry, so `focusSide`, `relativeDefocus` and `size` are disabled. If the object is at the focus distance, size is 0 and the node is identity. An object closer than the focal length clamps to the size maximum, as in ZDefocus.
- Unconfirmed: with `sizeFromLens` off, a traced lens on Defocus takes u = ±`relativeDefocus` (default 1 = a point at infinity) on the side `focusSide` picks (Behind/In Front), `size` sets the blur radius, and the Lens tab knobs build the table.
- Unconfirmed: distances on Defocus's Lens tab are in metres; focal length and film back are in mm, as in ZDefocus's Lens mode.
- Unconfirmed: M133's 32 px cell grid is fine enough for traced kernels. Their ρ variation is smooth, and lookups interpolate between table entries. M292.P3.T4 measures it and fixes it if not.
- Unconfirmed: the image stages run as one resampling pass after the defocus. Distortion, breathing and lateral CA form one combined warp per channel, and vignetting is a multiply on colour only.

## Phase 292.1: LensTrace engine

- [ ] M292.P1.T1 — Port LensTrace's table baking onto the shared optics library
  - files: `Engine/Nodes/Filter/LensTable.h`, `Engine/Nodes/Filter/LensTable.cpp` (from `git show niikl/RB-2.6:Engine/Dev/PostFX/LensTrace.{h,cpp}`), `Engine/CMakeLists.txt`, `Tests/Native/LensTable_Test.cpp` (new)
  - approach: Keep the GPL header and Niik-l's attribution. Port `LensTableParams`, `LensTable::build/kernel/lookup/tracePoint`, `TracedKernel`, `KernelLookup`, the field maps and `backFocusMm`/`effectiveFNumber`, but take prescriptions, focusing, per-wavelength refraction (catalogue Sellmeier, M462) and ray-surface intersection from `Engine/Optics` (M462) instead of LensTrace's own copies. No Natron or Qt types beyond `Engine/Optics`.
  - verify: `Tests/Native/LensTable_Test.cpp` checks that each Classic Designs lens focuses at 5 m and builds a table; that on axis at small aperture the traced blur radius matches the thin-lens CoC within 5%; that `kernel()` outlines agree with `tracePoint()` ray footprints (area IoU > 0.9 at ρ = 0 and ρ = 0.8); that a database aspheric lens builds a table. `ctest -R LensTable` at the phase build.
  - size: L

## Phase 292.2: Traced kernels in BokehKernel

- [ ] M292.P2.T1 — Rasterise traced kernels into BokehKernel
  - files: `Engine/Nodes/Filter/BokehKernel.h`, `Engine/Nodes/Filter/BokehKernel.cpp`
  - approach: Add `BokehKernel::fromTraced(const LensTrace::KernelLookup&, radiusPx, bladeShape, character)`. Rasterise the outline (iris polygon from blades/rotation, or a disc, through the 2×2 map) intersected with the clip discs. Shift it by coma, weight it by the ring profile, and renormalise to unit sum. `lensClipSoftness` feathers the clip-disc edges by that fraction of the kernel radius (0 = hard, as LensTrace). `character` lerps the map, shift, clips and profile towards identity. Then apply the stacked extras (onion rings, catadioptric, filter shape) exactly as for procedural kernels. Use the same quantisation as M133 so run counts stay bounded.
  - verify: tests in `Tests/Native/BokehKernel_Test.cpp`: character 0 equals the procedural kernel with the same iris (exact); an off-axis Petzval kernel has a cat-eye clip (asymmetric extents); per-channel kernels differ when lensColour is on; sums are 1 within 1e-6; clip softness 0 is bit-identical to the hard clip, and 0.2 gives a wider partial-weight band at the clip edge.
  - size: M

- [ ] M292.P2.T2 — Lens table cache
  - files: `Engine/Nodes/Filter/LensTableCache.h`, `Engine/Nodes/Filter/LensTableCache.cpp`
  - approach: A process-wide cache keyed by (prescription hash, focal length, f-stop, focus distance, field radius, lensColour). It holds `shared_ptr<const LensTable>`, is thread-safe, and LRU-bounded at 16 entries, since tracing costs milliseconds to seconds and renders call it per tile. A lens file is read via its path plus mtime.
  - verify: `Tests/Native/LensTrace_Test.cpp` case: the same key returns the same pointer; a different f-stop builds a new table; 8 threads requesting one key build it once.
  - size: M

- [ ] M292.P2.T3 — Iris from the aperture model
  - files: `Engine/Nodes/Filter/BokehKernel.h`, `Engine/Nodes/Filter/BokehKernel.cpp`, `Engine/Nodes/Filter/ZDefocus.{h,cpp}`, `Engine/Nodes/Filter/Defocus.{h,cpp}`
  - approach: Add M465 - Aperture Model's knob group (prefix `aperture`, minus the blade and rotation knobs the nodes already have, which drive it) to the Bokeh tab. When any grating, scratch, dust or image strength is non-zero, rasterise the iris with M465's CPU mask at the kernel's size and quantise as M133 does; otherwise keep today's polygon path. Parallax is ignored (bokeh has no light position).
  - verify: `Tests/Native/BokehKernel_Test.cpp` cases: all extras at 0 is bit-identical to the existing kernels; dust strength 1 changes the kernel and keeps its sum at 1 within 1e-6.
  - size: M

## Phase 292.3: Lens menu on ZDefocus and Defocus (CPU)

- [ ] M292.P3.T1 — ZDefocus Lens knobs and traced kernels (CPU)
  - files: `Engine/Nodes/Filter/ZDefocus.h`, `Engine/Nodes/Filter/ZDefocus.cpp`
  - approach: Add M463's shared lens knobs (`lensModel`, `lens`, `lensFile`, `glassVendor`, `abbeOffset`, `useAspheric`) plus `lensCharacter`, `lensColour` and `clampToLens` to the Bokeh tab, with labels from their script names. With a traced lens, disable M133's procedural catEye, swirl, CA and spherical knobs. Per slice × cell × channel, compute u from the slice's signed CoC divided by the CoC at infinity, and ρ from the cell centre. Fetch the kernel through `LensTableCache`, then `BokehKernel::fromTraced`. Apply `clampToLens`. A lens error sets a node error and passes the input through.
  - verify: `Tests/Native/NativeZDefocus_Test.cpp` cases: Procedural is bit-identical to the existing goldens; Double-Gauss renders differ between centre and corner cells; a bad lens file gives a persistent node error and identity output.
  - size: L

- [ ] M292.P3.T2 — Defocus Lens knobs and traced kernels (CPU)
  - files: `Engine/Nodes/Filter/Defocus.h`, `Engine/Nodes/Filter/Defocus.cpp`
  - approach: Add the same lens knobs to the Bokeh tab, and `focusSide` (Behind/In Front) and `relativeDefocus` (default 1). The table parameters come from the Lens tab knobs that M292.P3.T3 adds; until that task lands, use 50 mm, f/2.8, 36 mm film back and 5 m focus as constants. u = ±relativeDefocus, and the radius comes from `size`. Use M133's cell grid for ρ, with kernels cached per cell and channel.
  - verify: `Tests/Native/NativeDefocus_Test.cpp` cases: Procedural is bit-identical to the goldens; Behind and In Front give different kernels for a Petzval lens; a corner cell shows a cat-eye.
  - size: M

- [ ] M292.P3.T3 — Defocus Lens tab: optional size from lens geometry
  - files: `Engine/Nodes/Filter/Defocus.h`, `Engine/Nodes/Filter/Defocus.cpp`, `Engine/Nodes/Filter/DepthOfField.h`
  - approach: Add a second page, "Lens", with `sizeFromLens` (bool, default off, label "Size From Lens"), `focalLength` (mm, 50), `fStop` (2.8), `filmBackWidth` (mm, 36), `focusDistance` (m, 5) and `objectDistance` (m, 10). Labels follow the script names. When `sizeFromLens` is on, compute the signed CoC through `DepthOfField.h`'s Lens-mode function. Use the absolute value as the size in place of `size`, and the sign as the focus side. Disable `size`, `focusSide` and `relativeDefocus`. Use these knobs as the traced lens's table parameters in place of P3.T2's constants. The GPU path reads the resolved size, so it needs no shader change. With the toggle off, behaviour is unchanged.
  - verify: `Tests/Native/NativeDefocus_Test.cpp` cases: toggle off is bit-identical to the goldens; toggle on with 50 mm, f/2.8, 36 mm, focus 5 m, object 10 m gives the size `DepthOfField.h` predicts, matching a plain Defocus at that size exactly; object at the focus distance is identity; object in front picks the In Front traced kernel.
  - size: M

- [ ] M292.P3.T4 — Measure and remove cell seams
  - files: `Engine/Nodes/Filter/ZDefocus.cpp`, `Engine/Nodes/Filter/Defocus.cpp`, `Engine/Nodes/Filter/LensAberration.{h,cpp}` (the cell grid), `Tests/Native/NativeZDefocus_Test.cpp`
  - approach: Render a dense grid of point highlights at constant depth through Petzval (strong field variation) twice: on the 32 px cell grid and on a 4 px grid as the reference. If any point's kernel energy differs from the reference by more than 2% (L1 over its footprint), blend each pixel's kernel between its four nearest cells with bilinear weights. Blend the gathered results, not the kernels, so run tables stay per cell. Keep the cost under 2× the unblended gather. Record the measured seam error, and whether blending was needed, in `## Decisions`.
  - verify: the new test passes the 2% bound against the 4 px reference; Procedural mode is bit-identical to the goldens.
  - size: L

## Phase 292.4: Image stages

- [ ] M292.P4.T1 — Lens image-stage model
  - files: `Engine/Nodes/Filter/LensStages.h`, `Engine/Nodes/Filter/LensStages.cpp`
  - approach: From a `LensTable` and the stage amounts, build per-channel radial maps: source radius as a function of output radius, combining distortion, breathing (the table at the focus distance against infinity, scaled separately in x and y by the 2D `lensBreathing`) and lateral CA. Also build an illumination curve with `lensVignettingSpread` pulling the falloff inward while keeping the corner value. Relative mode normalises the curve to 1 at the centre. Absolute mode keeps the traced transmission, including iris masking and stop-down, so stopping down darkens the frame. Every amount at 0 means `isIdentity()`.
  - verify: `Tests/Native/LensTrace_Test.cpp` cases: all-zero amounts are identity; the Double-Gauss distortion sign matches the traced chief-ray height; spread keeps the corner illumination and darkens mid-field; Relative is 1 at the centre; Absolute at f/8 is darker at the centre than at f/2; breathing (1, 0) scales x only.
  - size: M

- [ ] M292.P4.T2 — Apply the image stages in ZDefocus and Defocus (CPU)
  - files: `Engine/Nodes/Filter/ZDefocus.h`, `Engine/Nodes/Filter/ZDefocus.cpp`, `Engine/Nodes/Filter/Defocus.h`, `Engine/Nodes/Filter/Defocus.cpp`
  - approach: Add `lensDistortion`, `lensBreathing`, `lensLateralCA`, `lensVignetting`, `lensVignettingSpread` and `lensVignettingMode` to the Bokeh tab, enabled only with a traced lens. After the defocus, one resampling pass per channel uses the existing resampler with the node's filter. Vignetting multiplies colour, not alpha. Enlarge the RoI by the warp's maximum source displacement. Skip the pass entirely when `isIdentity()`.
  - verify: native test cases: all stages at 0 are bit-identical to no stage; distortion 1 moves a grid of points by the model's radial map within 0.5 px; vignetting leaves alpha unchanged.
  - size: M

## Phase 292.5: GPU

- [ ] M292.P5.T1 — Traced kernels on the GPU
  - files: `Engine/Nodes/Filter/ZDefocus.cpp`, `Engine/Nodes/Filter/Defocus.cpp`, `Engine/Gpu/Kernels/zdefocus.slang`, `Engine/Gpu/Kernels/defocus.slang`
  - approach: Traced kernels are ordinary `BokehKernel` runs, so feed them through M133's per-cell, per-channel run tables in storage buffers. Kernels stay on the GPU under the same size and scratch limits as M133, with CPU fallback over them. No shader logic is needed beyond what M133 has unless the parity tests show otherwise.
  - verify: `Tests/Native/NativeZDefocusGpu_Test.cpp` and `NativeDefocusGpu_Test.cpp` parity cases with a Double-Gauss lens, lensColour on, pass on RADV at the phase build.
  - size: M

- [ ] M292.P5.T2 — Image stages on the GPU
  - files: `Engine/Gpu/Kernels/lensstages.slang`, `Engine/Gpu/Kernels/lensstages.cmake`, `Engine/Nodes/Filter/ZDefocus.cpp`, `Engine/Nodes/Filter/Defocus.cpp`
  - approach: One compute pass reads per-channel radial maps (a 1D lookup per channel in a storage buffer) and the illumination curve, then resamples bilinear or with the node's filter as the CPU does. Follow the `defocus.cmake` registration pattern. Chain it after the defocus pass with no CPU round-trip.
  - verify: GPU parity cases for each stage at 1, within the existing parity tolerance, on RADV at the phase build.
  - size: M

- [ ] M292.P5.T3 — GPU table bake
  - files: `Engine/Gpu/Kernels/opticsTrace.slang` (new, a reusable sequential tracer include), `Engine/Gpu/Kernels/lenstable.slang` (new), `Engine/Gpu/Kernels/lenstable.cmake` (new), `Engine/Gpu/GpuLensTable.{h,cpp}` (new), `Engine/Nodes/Filter/LensTableCache.{h,cpp}`, `Tests/Native/LensTableGpu_Test.cpp` (new)
  - approach: Port the sequential trace that `LensTable::build` runs (sphere, plane and asphere intersection, refraction with M462's per-surface, per-wavelength IORs uploaded as a buffer) into `opticsTrace.slang`, written as an include that M467 - LensFlare GPU Ghosts can reuse. `lenstable.slang` runs one thread per (u, ρ, channel, ray) and reduces to LensTable's kernel fields on the GPU. The 2×2 map, clip discs, coma shift and ring profile come back as a small buffer that `LensTableCache` stores. Use the GPU bake when a device is present, else the CPU. Register the pass following `defocus.cmake`.
  - verify: `LensTableGpu_Test` compares GPU and CPU tables for every Classic Designs lens: outline IoU > 0.98 at ρ = 0, 0.5 and 0.9, ring profiles within 1e-3, coma shift within 0.01 of the kernel radius. It skips without a device (`reportNoGpu`). Runs on RADV at the phase build.
  - size: L

## Phase 292.6: Verification

- [ ] M292.P6.T1 — Lens control audit
  - files: `docs/defocus/lens-controls-audit.md`
  - approach: Make a table of every lens-related LensDefocus control: Niik-l's script name, our knob, default and node (ZDefocus/Defocus). Read the controls from `git show niikl/RB-2.6:Engine/Dev/PostFX/LensDefocus.cpp` (lines ~870–960). No row may say MISSING; each control the user ruled out says which decision ruled it out.
  - verify: every Bokeh-tab lens control in Niik-l's source appears in the table, with no MISSING rows.
  - size: S

- [ ] M292.P6.T3 — Bokeh preview in the lens browser
  - files: `Gui/LensBrowser.{h,cpp}` (from M463), `Engine/Nodes/Filter/LensTable.{h,cpp}`
  - approach: Register a bokeh preview provider with M463's browser: for the selected lens, render the traced kernel at centre and corner (ρ = 0 and 0.8) with the calling node's f-stop and lens colour, on a worker thread, cancelled when the selection changes.
  - verify: the GUI script in P6.T2 opens the browser from ZDefocus, selects two lenses and screenshots a visible, differing bokeh preview for each.
  - size: M

- [ ] M292.P6.T2 — Goldens, docs and GUI check
  - files: `Tests/Native/goldens/ZDefocus/`, `Tests/Native/goldens/Defocus/` (new traced-lens goldens), `Engine/Nodes/Filter/docs/ZDefocus.md`, `Engine/Nodes/Filter/docs/Defocus.md`, `Tests/gui/m292_traced_lens.py`
  - approach: Goldens on the parity source for Double-Gauss, Petzval with lensColour on, and the stages at 1, as ≤256 px half-float crops. Docs list the Classic Designs lenses with their citations, point to the lens browser, the lens-file format, and how traced and procedural knobs interact. The GUI script also toggles Defocus's Size From Lens and checks that `size` is disabled. It renders a point-light grid with a depth ramp under Xvfb through each built-in lens, centre and corner, and takes screenshots.
  - verify: `ctest -R "NativeZDefocus|NativeDefocus"` passes; the GUI script passes; screenshots are shared with the user.
  - size: M

- [ ] M292.P6.T4 — Focus-rack timing
  - files: `tools/bench/zdefocus_focus_rack.py` (new)
  - approach: In NatronRenderer, run a 2K ZDefocus on a point-highlight plate with a depth ramp through Double-Gauss with lensColour on. Animate `focalPlane` over 48 frames, so every frame needs a new table. Log the bake time per frame from `LensTableCache`'s timing counter (add one if missing, debug-log only), and the frames per second. Run it on the GPU host per the bench rules (before and after in one sitting is not needed: this is a new capability), and once with the GPU disabled for the CPU fallback.
  - verify: on the GPU host the median bake is ≤ 30 ms and the render holds ≥ 10 fps. Record both numbers and the CPU fallback's numbers in `## Decisions`.
  - size: M

**Verification gate:** the full build and `ctest` pass, with GPU parity on RADV. With Lens Model = Procedural, the existing M131/M133 goldens are unchanged. The lens control audit has no MISSING rows. The cell-seam test passes, and the focus-rack timing meets ≤ 30 ms per bake and ≥ 10 fps at 2K on the GPU host, with its numbers recorded. The GUI check passes and its screenshots are shared with the user, who OKs them before sign-off. An AppImage built with `package.sh` is handed over for UAT.

## Decisions

- 2026-11-01 — **Port LensTrace into our nodes, not Niik-l's LensDefocus node (user, /cat-plan grill):** "Their node is a bit rough around the edges; probably we shouldn't port it exactly. The most interesting part is the lens tracing work."
- 2026-11-01 — **All four image stages, opt-in (user):** distortion, breathing, lateral colour and vignetting (with spread), each defaulting to 0.
- 2026-11-01 — **ZDefocus and Defocus get the Lens menu (user):** Defocus adds Focus Side; IDefocus inherits through BokehKernel with no new knobs.
- 2026-11-01 — **Defocus keeps `size` as the primary control, plus an optional Lens tab (user):** "a second tab that optionally sets the size from lens/f-stop/filmback/focus distance would actually be really nice." `objectDistance` was added because a CoC needs the subject's distance as well as the focus distance.
- 2026-11-01 — **A traced lens replaces the overlapping procedural aberrations, and the extras stack (user):** blades/rotation shape the iris; onion rings, catadioptric, filter shape and highlight gain stack on top.
- 2026-11-01 — **GPU residency (user):** the user asked for fully GPU-resident defocus, so traced kernels and image stages run on the GPU with no CPU round-trip.
- 2026-11-16 — **Built on the shared lens base (user, lens flare planning):** prescriptions, parsers, dispersion and tracing come from M462 - Shared Optics Library, lens selection from M463 - Bundled Lens Database, the iris from M465 - Aperture Model; this milestone stays in F13 - ZDefocus Rework and waits for F30 - Lens Optics Base to land.
- 2026-11-16 — **Catalogue Sellmeier dispersion for bokeh too (user):** each surface's glass is matched to the nearest catalogue glass, replacing LensTrace's Cauchy estimate, so bokeh and flares agree.
- 2026-11-16 — **Lens Model is Procedural / Database / From File (user):** the 9 built-ins move into the database's Classic Designs group.
- 2026-11-16 — **Iris from the aperture model, and a bokeh preview in the lens browser (user).**
- 2026-11-16 — **Match or exceed Higx Optics (user, /cat-plan grill):** "the traced defocus work needs parity with this closed-source project … ensure we're matching at least what they're delivering or that we exceed it." This milestone takes the GPU bake, vignetting modes, clip softness and two-axis breathing; M473–M479 take the rest.
- 2026-11-16 — **Interactive focus racking (user):** GPU table bake; ≤ 30 ms per bake and ≥ 10 fps for a 2K focus drag on the GPU host, CPU fallback recorded.
- 2026-11-16 — **Traced lenses stay on ZDefocus and Defocus (user, reopened and kept):** a separate optical-defocus node was considered and rejected; Kernel Type = Image bypasses the lens.
- 2026-11-16 — **No extra layer inputs, glitter or rain-on-glass layer (user).**
- 2026-11-16 — **`filmBackWidth` on Defocus's Lens tab (planner):** the same script name as ZDefocus, so M473 - Lens Camera And Focus Controls' film formats drive both.
