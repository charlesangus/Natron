# M476 - Traced Anamorphic Bokeh

True anamorphic bokeh from traced prescriptions with cylinder (and, after M478 - Cinema And Vintage Lens Prescriptions, biconic or toroidal) surfaces (user, 2026-11-16). This exceeds the reference product, which traces spherical lenses only and fakes anamorphics with a squeeze factor. A non-rotationally-symmetric lens can't be described by the field radius alone, so M292 - Traced Lens Bokeh's table gets a 2D field grid. The image stages get per-axis maps. An `anamorphicPlate` choice says whether the plate is still squeezed or already desqueezed.

Deps: M292 - Traced Lens Bokeh.

## Scope

**In:**
- 2D field tables: when a prescription has any non-rotationally-symmetric surface, bake over a field grid in (hx, hy), folded into one quadrant by its mirror symmetry about x and y. Lookups are bilinear. Rotationally symmetric lenses keep the 1D table.
- The squeeze factor from the paraxial focal lengths in x and y (`Optics::Paraxial` per meridian).
- `anamorphicPlate` (label "Anamorphic Plate") on ZDefocus and Defocus: Auto (default; a pixel aspect other than 1 means a squeezed plate, a pixel aspect of 1 means desqueezed), Squeezed, Desqueezed (user, 2026-11-16: Auto with an override). Squeezed keeps kernels in sensor space, and the format's pixel aspect does the rest. Desqueezed stretches kernels horizontally by the lens's squeeze.
- Per-axis distortion, breathing and lateral CA in M292's image stages (separable x and y maps) for anamorphic lenses, CPU and GPU.
- The GPU bake over the 2D grid.

**Out:**
- Biconic and toroidal surface types: M478 - Cinema And Vintage Lens Prescriptions adds them; this milestone works with cylinders and picks the new types up through the shared tracer.
- Anamorphic lens flares (horizontal streaks): F31 - Lens Flares traces them through the same library; nothing here.

**Edge cases:**
- A rotationally symmetric lens: the 1D path, bit-identical to M292.
- Auto with a pixel aspect between 1 and the lens's squeeze (e.g. 1.33 on a 2× lens): treated as squeezed, with the kernel in sensor space. The docs say to override when a plate was partly desqueezed.
- Squeeze factor 1 from a lens whose cylinders cancel: the 2D table still runs, and kernels come out round.
- `aspectRatio` (existing) still multiplies on top, as a creative stretch.

**Assumptions:**
- Unconfirmed: a 9×9 quadrant field grid is fine enough; the seam test from M292.P3.T4 runs on an anamorphic lens to confirm it.
- Unconfirmed: OpticsDatabase contains at least one photographic prescription with cylinder surfaces; otherwise the tests use a synthetic fixture (a spherical prime behind a 2× cylindrical front group).

## Phase 476.1: 2D field tables

- [ ] M476.P1.T1 — 2D field bake and lookup (CPU)
  - files: `Engine/Nodes/Filter/LensTable.{h,cpp}`, `Engine/Optics/Paraxial.{h,cpp}`, `Tests/Native/LensTable_Test.cpp`, `Tests/fixtures/optics/anamorphic_2x.zmx` (new, synthetic, small)
  - approach: Detect non-rotationally-symmetric prescriptions. Bake kernels over a 9×9 grid of (hx, hy) in [0, 1]² for each u and channel, and mirror lookups into the quadrant (mirroring the kernel's 2×2 map, clip discs and coma shift with it). Add per-meridian paraxial focal lengths and `squeeze()`. Remove M292's "anamorphic lens" node error.
  - verify: tests: a spherical lens baked in 2D matches its 1D table within 1e-4 at matching radii; the 2× fixture gives an on-axis kernel whose extents ratio is 2 within 3% and whose squeeze() is 2 within 1%; mirrored lookups at (−hx, hy) mirror the (hx, hy) kernel.
  - size: L

- [ ] M476.P1.T2 — 2D field bake on the GPU
  - files: `Engine/Gpu/Kernels/lenstable.slang`, `Engine/Gpu/GpuLensTable.{h,cpp}`, `Tests/Native/LensTableGpu_Test.cpp`
  - approach: Extend the GPU bake's dispatch to the 2D grid when the table is 2D, with the same reduction.
  - verify: GPU and CPU 2D tables for the 2× fixture agree within M292's `LensTableGpu_Test` bounds on RADV at the phase build.
  - size: M

## Phase 476.2: Plates and stages

- [ ] M476.P2.T1 — Anamorphic Plate choice
  - files: `Engine/Nodes/Filter/ZDefocus.{h,cpp}`, `Engine/Nodes/Filter/Defocus.{h,cpp}`, `Engine/Nodes/Filter/BokehKernel.{h,cpp}`, `Tests/Native/NativeZDefocus_Test.cpp`
  - approach: Add `anamorphicPlate` (Auto, Squeezed, Desqueezed), shown only for an anamorphic traced lens. Resolve Auto from the output format's pixel aspect. For Desqueezed, scale the rasterised kernel horizontally by `squeeze()`, and map positions to the 2D field accounting for the desqueeze.
  - verify: tests: with the 2× fixture, a PAR 2 format under Auto equals Squeezed exactly; a PAR 1 format under Auto equals Desqueezed exactly; Desqueezed kernels are twice as wide as Squeezed in pixels.
  - size: M

- [ ] M476.P2.T2 — Per-axis image stages
  - files: `Engine/Nodes/Filter/LensStages.{h,cpp}`, `Engine/Gpu/Kernels/lensstages.slang`, `Tests/Native/LensTrace_Test.cpp`
  - approach: For a 2D table, build separable x and y source maps per channel (distortion, breathing, lateral CA) from the traced chief rays along each axis. Rotationally symmetric lenses keep the radial maps.
  - verify: tests: a spherical lens gives the same warp through both paths within 1e-4 px; the 2× fixture's breathing scales x and y by different traced amounts; GPU parity.
  - size: M

## Phase 476.3: Docs and GUI

- [ ] M476.P3.T1 — Goldens, docs and GUI check
  - files: `Tests/Native/goldens/ZDefocus/` (anamorphic golden), `Engine/Nodes/Filter/docs/ZDefocus.md`, `Engine/Nodes/Filter/docs/Defocus.md`, `Tests/gui/m476_anamorphic.py` (new)
  - approach: A golden of the 2× fixture on a point grid, desqueezed. Docs explain the 2D table, the plate choice and Auto's rule. The GUI script screenshots a squeezed (PAR 2) and a desqueezed render.
  - verify: `ctest -R "NativeZDefocus|LensTable|LensTrace"` passes; the GUI script passes, and its screenshots are collected for F13 - ZDefocus Rework's UAT.
  - size: M

**Verification gate:** the full build and `ctest` pass, with GPU parity on RADV; rotationally symmetric lenses are bit-identical to M292 - Traced Lens Bokeh; M292's seam test passes on the 2× fixture; the GUI screenshots are collected at F13 - ZDefocus Rework's UAT.

## Decisions

- 2026-11-16 — **Trace anamorphics truly (user, /cat-plan grill):** a 2D field table rather than a node error plus a squeeze knob.
- 2026-11-16 — **Anamorphic Plate: Auto from the pixel aspect, with an override (user).**
