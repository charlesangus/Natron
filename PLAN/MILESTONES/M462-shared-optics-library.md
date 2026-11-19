# M462 - Shared Optics Library

An engine-free optics library, `Engine/Optics`, shared by traced bokeh (M292 - Traced Lens Bokeh) and lens flares (F31 - Lens Flares): lens prescriptions with spherical, aspheric and cylindrical surfaces; `.zmx` and pbrt/Kolb `.dat` parsers; real glass catalogues with Sellmeier dispersion; a CPU sequential tracer and a two-bounce ghost tracer per wavelength; Fresnel and single-layer anti-reflection coatings; paraxial lens facts; and spectral colour into the working space. It is the CPU reference every GPU path later matches.

Sources (both GPL-compatible with the GPLv3-or-later relicence; record them in file headers): realflare (`https://github.com/beatreichenbach/realflare`, GPL-3.0-or-later, v1.0.3 at 9ce30b0): `flare/engine/tasks/shaders/raytracing.comp`, `flare/api/lens/model.py`, `flare/adapters/database/parsers/zemax.py`, `flare/api/color/`, `flare/engine/tasks/common/raytracing.py`. Niik-l's LensTrace (`niikl/RB-2.6:Engine/Dev/PostFX/LensTrace.{h,cpp}`, GPL-2.0-or-later): the 9 built-in prescriptions with their patent and book citations, `layout()`, the `.dat` parser. Glass data: the OpticsDatabase `.mtrl` files (MIT, `https://github.com/amegahed/OpticsDatabase`, credits Mikhail Polyanskiy / refractiveindex.info).

## Scope

**In:**
- `Optics::Prescription` (surfaces: Stop, Standard sphere or plane, Even Asphere, Extended Asphere, Extended Odd Asphere, Cylinder X, Cylinder Y; radius, curvature, spacing, clear semi-diameter, conic, up to 8 coefficients, nd, Vd), with Niik-l's 9 built-ins.
- `.zmx` parsing of every field realflare reads, and pbrt/Kolb `.dat` with an optional Abbe column.
- Glass catalogues for Schott, Ohara, Hoya, CDGM, Hikari and Sumita (Sellmeier formulas 1, 2 and 3), converted once into a small committed JSON; nearest-glass matching by relative (nd, Vd) error with an Abbe offset.
- Sequential and ghost tracing per wavelength: ray-surface intersection (sphere, plane, asphere by Newton, cylinder), refraction, reflection, total internal reflection, the housing ratio (`rrel`) and the normalised stop position.
- Fresnel reflectance with a single-layer thin-film AR coating per surface (Ritschel 2009), Schlick on uncoated surfaces, and seeded coating generation.
- Paraxial effective focal length, f-number, back focus and focusing to a distance.
- Spectral colour: CIE 1931 2° standard observer CMFs, the CIE illuminant set, and wavelength → working-space RGB through the project's OCIO config.

**Out:**
- Bokeh tables: M292 - Traced Lens Bokeh.
- The lens database and browser: M463 - Bundled Lens Database.
- GPU tracing: M467 - LensFlare GPU Ghosts.
- Toroids, biconics, GRIN, tilts and decentres, polarisation, glass absorption, Zemax multi-configuration (zoom positions): none.

**Edge cases:**
- A `.zmx` or `.dat` that fails to parse: an error naming the line; callers turn it into a node error.
- A surface whose nd/Vd has no catalogue glass within tolerance: Cauchy-like dispersion from nd and Vd, as LensTrace does.
- Asphere Newton solve that fails to converge in 12 iterations: the ray misses (invalid), as in realflare.
- A glass with no Vd in the `.zmx`: Vd = 60, as in realflare.
- Coatings: none on the stop, on the last surface, or where the medium on both sides is the same.

**Assumptions:**
- Unconfirmed: focal length and f-number come from the paraxial trace (`FNUM` overrides the f-number when present) instead of realflare's file-name regex.
- Unconfirmed: the full 8 aspheric coefficients are used on the CPU; the GPU (M467) may keep realflare's 4 if parity tests show no visible difference.

## Phase 462.1: Prescriptions and parsers

- [x] M462.P1.T1 — Prescription model, `.dat` parser and the 9 built-ins
  - files: `Engine/Optics/Prescription.{h,cpp}` (new), `Engine/Optics/DatParser.{h,cpp}` (new), `Engine/Optics/BuiltinLenses.cpp` (new), `Engine/CMakeLists.txt`, `Tests/Native/OpticsPrescription_Test.cpp` (new)
  - approach: Namespace `Natron::Optics`, no Natron or Qt types. Model the surface types and fields listed in Scope, in mm. Port Niik-l's 9 built-in prescriptions, citations and `layout()` (surface vertex positions and centres of curvature) from `git show niikl/RB-2.6:Engine/Dev/PostFX/LensTrace.cpp`, keeping the GPL header. Port the `.dat` parser (radius thickness index aperture [abbe]); a missing stop is an error.
  - verify: gtest: every built-in parses into a prescription with exactly one stop; a pbrt Double-Gauss `.dat` round-trips its surfaces; a `.dat` with no stop fails with a line number.
  - size: M

- [x] M462.P1.T2 — Zemax `.zmx` parser
  - files: `Engine/Optics/ZemaxParser.{h,cpp}` (new), `Tests/Native/OpticsZemax_Test.cpp` (new), `Tests/fixtures/optics/` (two small `.zmx` files: a spherical prime and an aspheric zoom position)
  - approach: Port realflare's `zemax.py`: `SURF`, `TYPE` (STANDARD, EVENASPH, XASPHERE, XOSPHERE, plus the custom STOP, CYLINDER_X, CYLINDER_Y), `STOP`, `CURV`, `DISZ` (INF-spaced surfaces dropped), `GLAS` (`___BLANK 1 0 nd Vd`, Vd defaulting to 60), `DIAM`, `CONI`, `PARM 1–8`, `XDAT` (index ≥4 for XASPHERE, ≥3 for XOSPHERE), `NOTE`, `FNUM`. Handle UTF-16 files, which some Zemax exports use.
  - verify: gtest on the fixtures: surface count, curvatures, conics and coefficients match values read by hand from the files; a truncated file fails with a line number.
  - size: M

## Phase 462.2: Glass and dispersion

- [x] M462.P2.T1 — Glass catalogues and Sellmeier dispersion
  - files: `tools/optics/convert_glass.py` (new), `Gui/Resources/Optics/glass.json` (new, generated, committed), `Gui/Resources/Optics/LICENSE-OpticsDatabase.txt` (new), `App/CMakeLists.txt` (copy `Gui/Resources/Optics` to `<bin>/../Resources/optics` and install it, following the TabTabTab resources pattern), `Engine/Optics/Glass.{h,cpp}` (new), `Tests/Native/OpticsGlass_Test.cpp` (new)
  - approach: The script reads the six vendors' `.mtrl` files from a pinned OpticsDatabase commit (argument: a local checkout) and writes name, nd, Vd and the formula coefficients. `Glass::ior(lambdaNm)` evaluates formulas 1, 2 and 3 as realflare's `Material.get_ior`. `matchGlass(vendor, nd, Vd, abbeOffset)` picks the nearest by relative error, falling back to a Cauchy-like estimate from nd and Vd beyond a tolerance. Air is 1. The loader finds `Resources/optics/glass.json` next to the binary as TabTabTab's resources are found.
  - verify: gtest: Schott N-BK7 at 587.6 nm gives 1.5168 within 1e-4; matching (1.5168, 64.17) in Schott returns N-BK7; abbeOffset 20 picks a different glass; a far-off (nd, Vd) uses the fallback. The committed JSON is under 1 MB.
  - size: M

## Phase 462.3: Tracing

- [x] M462.P3.T1 — Sequential and ghost tracer
  - files: `Engine/Optics/Trace.{h,cpp}` (new), `Tests/Native/OpticsTrace_Test.cpp` (new)
  - approach: `Optics::TraceSystem` is built from a prescription, a glass vendor, an Abbe offset and a wavelength list (per-surface IOR per wavelength). `traceSequential(ray, λ)` and `traceGhost(ray, λ, bounce1, bounce2)` follow realflare's `raytracing.comp`: forward to `bounce1`, reflect back to `bounce2`, reflect again, forward to the sensor. Intersections: sphere, plane, asphere by 12-step Newton on z(r) = c r²/(1+√(1−(1+k)c²r²)) + Σ aᵢ rⁱ, cylinders by quadratic. Track `rrel` (max |xy| / clear semi-diameter), the stop position divided by the stop height, and invalidate on a miss or total internal reflection. `enumerateGhosts()` returns the bounce pairs realflare's `get_paths` does (no pair spanning the stop). A `useAspheric` flag traces aspheres as spheres.
  - verify: gtest: a paraxial ray through a single biconvex lens lands at the thin-lens focus within 0.5%; an on-axis ray through any built-in passes undeviated; an aspheric fixture's Newton solve converges; a 10-surface lens gives the expected ghost-pair count; a ray past the clear aperture gives rrel > 1.
  - size: L

- [x] M462.P3.T2 — Fresnel and anti-reflection coatings
  - files: `Engine/Optics/Coating.{h,cpp}` (new), `Engine/Optics/Trace.cpp`, `Tests/Native/OpticsTrace_Test.cpp`
  - approach: Port realflare's `fresnel_ar` (single-layer thin film, s and p amplitudes, design wavelength and coating IOR, nc = max(√(n1·n2), coatingIor), d = λ_design / (4 nc)) and Schlick for uncoated surfaces. `generateCoatings(prescription, seed, wavelengthRange, iorRange)` draws per-surface (wavelength, IOR) from a PCG stream, skipping the stop, the last surface and same-medium surfaces. Ghost rays carry the product of their two reflectances.
  - verify: gtest: reflectance at normal incidence matches ((n1−n2)/(n1+n2))² for an uncoated surface within 1e-6; a quarter-wave coating at its design wavelength reflects less than uncoated; the same seed gives the same coatings; the stop gets none.
  - size: M

- [x] M462.P3.T3 — Paraxial facts and focusing
  - files: `Engine/Optics/Paraxial.{h,cpp}` (new), `Tests/Native/OpticsTrace_Test.cpp`
  - approach: ABCD (y-nu) trace for effective focal length, back focal distance, entrance pupil and f-number (stop semi-diameter at the entrance pupil; `FNUM` overrides); `focusTo(distanceMm)` moves the image plane, as LensTrace's focusing does.
  - verify: gtest: Double-Gauss EFL within 1% of its citation's value; f-number matches the `.zmx` `FNUM` fixture within 5%; focusing to 5 m moves the image plane by the Newtonian amount within 1%.
  - size: M

## Phase 462.4: Spectral colour

- [x] M462.P4.T1 — Wavelengths to working-space RGB
  - files: `Engine/Optics/Spectral.{h,cpp}` (new), `Engine/Optics/SpectralData.cpp` (new: CIE 1931 2° CMFs 360–830 nm at 1 nm, the CIE illuminants A, B, C, D50, D55, D60, D65, D75, E and FL1–FL11), `Tests/Native/OpticsSpectral_Test.cpp` (new)
  - approach: Port realflare's `get_spectral`: CMF × illuminant × 0.01 → XYZ, then XYZ (D65) → the working space through the project's OCIO config (`cie_xyz_d65_interchange` role to `scene_linear`), with a fixed matrix fallback to linear Rec.709 when the config has no such role. `sampleWavelengths(count)` returns mid-bin positions over 390–730 nm.
  - verify: gtest: integrating equal-energy illuminant E over all wavelengths gives a neutral colour within 1e-3 in linear Rec.709; D65 integrated gives white within 1e-3; 5 sampled wavelengths are at the mid-bin positions.
  - size: M

**Verification gate:** build and `ctest -R Optics` green; the converter script reruns from a pinned OpticsDatabase checkout to a byte-identical `glass.json`; no Natron/Qt includes under `Engine/Optics` (grep).

## Decisions

- 2026-11-16 — **One shared optics library for bokeh and flares (user, /cat-plan grill):** "we should seek to synergize this work"; M292 - Traced Lens Bokeh builds on it.
- 2026-11-16 — **Catalogue Sellmeier dispersion for both nodes (user):** nearest catalogue glass per surface, Glass Vendor and Abbe Offset knobs; Cauchy only as the fallback.
- 2026-11-16 — **Three Features (user):** this milestone, M463 - Bundled Lens Database and M465 - Aperture Model form F30 - Lens Optics Base; the bokeh work stays in F13 - ZDefocus Rework and the flares form F31 - Lens Flares, both waiting for F30 to land.
- 2026-11-19 (user): spectral colour is physically correct by default: spectra integrate against colour-matching functions consistent with the working space's observer, so D65 renders neutral in a D65-white space and other illuminants (E included) keep their true tint. A Local White mode renders the chosen illuminant exactly neutral in the actual working space, luminance preserved. The ported method was ~1.5% off white on D65 (2015 CMFs against a 1931-defined space).
- 2026-11-19: spectral fix at a9a7167b5: CIE 1931 2° observer (colour-science 0.4.6 data, BSD-3-Clause), 360–830 nm because 390–730 leaves D65 ~1e-3 off; OCIO's D65 adaptation is undone for ACES-white working spaces; a working space whose interop ID isn't recognised is treated as D65. D65 renders neutral in Rec.709 (1e-4), D60 in ACEScg; Local White makes E, D65 and A neutral.
- 2026-11-19: all tasks done at a9a7167b5. The gate is green (rerun 44565699520: the legacy leg's first failure was an unrelated MoonRay projection timeout). convert_glass.py at the pinned OpticsDatabase commit gives a byte-identical glass.json (1534 glasses). Engine/Optics has no Natron or Qt includes.
