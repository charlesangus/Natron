# M457 - ColorWarp Space Spike

First member of F28 - Colour Warp. ColorWarp lets the user pick colours in the viewer and push them through colour space like warp pins, building a smooth warp of the colour volume from many pins. Which space the pins live in and how their shifts spread is the open design question (user, 2026-11-16: "we'll need to experiment… probably a perceptual space, maybe XYZ with a translation layer" — meaning shifts presented to the user as HSV/RGB offsets relative to the picked colour, with the warp itself stored in XYZ). This milestone builds the real warp-field library with every candidate, renders comparisons, and puts the choice to the user. The library is kept and used by M458 - ColorWarp Node; it is not throwaway spike code.

## Scope

**In:**
- `ColorWarpField`: a pure library (no Qt, no OCIO; matrices passed in as doubles, like `Engine/ColorWheelMath`) that, given pins (source colour, target colour, strength, falloff, enabled), maps a working-space RGB colour to its warped colour.
- Candidate spaces, each a pair of conversions from and to the scene-linear working space: OKLab on a log encoding; CIE XYZ storage with shifts authored as HSV/RGB offsets relative to the pin's source (the user's idea); log RGB; linear RGB.
- Candidate kernels: a normalised Gaussian falloff blend (Shepard-style, where hold pins — pins with no shift — pull nearby colours back to identity) and a regularised 3D polyharmonic RBF with an affine part (exact through every pin).
- A comparison harness rendering synthetic charts (colour checker patches, HDR ramps to 16+, a skin-tone patch, a neutral ramp) through every space × kernel with the same pin sets, written as images under `build/` (never fixtures).
- A short report and the user's pick of default space and kernel, recorded as a decision for M458 - ColorWarp Node.

**Out:**
- The node, its knobs and viewer interaction — M458 - ColorWarp Node.
- The 3D view — M459 - ColorWarp 3D View.
- GPU code — M458 - ColorWarp Node.

**Edge cases:**
- Negative and above-1 scene-linear values: every conversion round-trips them (log encodings get a defined linear toe below 0); tests cover −0.05 and 64.
- No pins, or all disabled: identity, bit-exact.
- Coincident sources: the RBF solve is regularised and never fails; the Gaussian blend averages.
- A hold pin exactly on a moved pin's source: the moved pin wins at its source; documented.

**Assumptions:**
- Pin counts stay in the tens to low hundreds, so a dense solve per change is cheap; no cap is imposed (none is planned).

## Phase 457.1: Library

- [x] M457.P1.T1 — Warp spaces
  - files: `Engine/Nodes/Color/ColorWarp/ColorWarpSpace.{h,cpp}` (new), `Engine/CMakeLists.txt`, `Tests/Native/ColorWarpSpace_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: an enum of the four candidate spaces and pure functions `toWarpSpace` / `fromWarpSpace` taking the working space's RGB→XYZ matrix as doubles; for the XYZ candidate, also `shiftFromHsvRgbOffset` turning an HSV or RGB offset relative to a source colour into an XYZ displacement and back. Doubles throughout, never clamped.
  - verify: gtest — round trips to 1e-9 for in-range, negative and HDR samples in every space; the HSV offset helper reproduces a known hue rotation on a saturated colour.
  - size: M

- [x] M457.P1.T2 — Warp field kernels
  - files: `Engine/Nodes/Color/ColorWarp/ColorWarpField.{h,cpp}` (new), `Engine/CMakeLists.txt`, `Tests/Native/ColorWarpField_Test.cpp` (new)
  - approach: `ColorWarpField::prepare(pins, space, kernel)` precomputes (for the RBF: an (n+4)² regularised solve of the polyharmonic r or r³ kernel with an affine part, per displacement axis; for the Gaussian blend: per-pin weights and displacements) and `apply(rgb)` evaluates one colour. Strength scales each pin's displacement; falloff is the kernel's reach in warp-space units; hold pins contribute zero displacement with full weight. Keep the evaluated form plain (arrays of floats) so M458's Slang kernel can evaluate the same thing.
  - verify: gtest — identity with no pins; each kernel maps a lone pin's source to its target at strength 1 and half-way at 0.5; colours far beyond the falloff are unchanged by the Gaussian blend; a hold pin keeps its colour fixed while a neighbouring pin moves; coincident pins don't fail.
  - size: L

## Phase 457.2: Comparison

- [x] M457.P2.T1 — Comparison harness
  - files: `Tests/Native/ColorWarpCompare.cpp` (new, a test or small tool target disabled by default in ctest), `Tests/CMakeLists.txt`
  - approach: generate the synthetic charts in code, define three pin sets (push a single hue, push skin warmer with a hold pin on a neutral, a five-pin "look"), and write one half-float EXR or PNG per space × kernel × pin set under `build/colorwarp-compare/`, labelled in the filename; also print a smoothness metric (maximum local gradient of the warp over a lattice) per combination.
  - verify: running it writes the full image set and the metric table.
  - size: M

- [x] M457.P2.T2 — Report and user pick
  - files: report only (`build/colorwarp-compare/REPORT.md`)
  - approach: summarise how each combination behaves (hue shift evenness across exposure, HDR stability, smoothness metric, hold-pin behaviour), recommend a default space and kernel, and send the images to the user one file each with the report; record the user's pick in this file's Decisions and set `Needs: response` while waiting.
  - verify: the decision is recorded and names the default space and kernel, and whether the other candidates remain as a knob option in M458 - ColorWarp Node.
  - size: M

**Verification gate:** build and ctest green; the user has picked the default space and kernel from the comparison images, and the pick is recorded here.

## Decisions

- 2026-11-19 (user, after the spike images and report at build/colorwarp-compare/REPORT.md in the M457 worktree): ColorWarp exposes every space and an RBF kernel option for the user to try, and the choices may be cut before shipping. Spaces: OKLab-log offered in both forms, log2 in place of the cube root (linear toe below 2^-7) and the plain cube root, plus Log RGB, XYZ and linear RGB. Kernels: the exact-at-pin Gaussian, and RBF (r, r³) as an option. Default space and kernel: OKLab-log (log2) with the Gaussian, as recommended. A global Lightness Falloff knob stretches a pin's reach along lightness (elliptical reach); its default keeps the ±1 stop reach. Strength 0 makes a hold pin. Each space's falloff is normalised (calibrated 0.93, 1.81, 0.11, 0.22 in the spike) so the same knob value reaches about as far in every space.
- 2026-11-19: done at 9488fa5de. Spike recommendation: OKLab-log + Gaussian, Log RGB second; RBF is global (one pin moves 96-98% of pixels with five pins) and linear spaces are 5-30x rougher per log stop.
