# M160 - Lensfun Lens Correction

A native LensCorrect node that corrects distortion, transverse chromatic aberration and vignetting from the lensfun database, ported from darktable `src/iop/lens.cc`. It is image in, image out, and works on any image: after a raw Develop chain, or on a JPEG, TIFF or EXR still. Camera, lens, focal length, aperture and focus distance fill in from metadata when present (`raw/*` from M80 - Native RAW Support, `exif/*` from the native Read), and are typed by hand otherwise. Geometry runs on M113 - Native Warp Nodes' per-pixel-coordinate sampler (`resampleRowFromCoords` and `coordmap.slang`), so CPU and GPU share one path. It stays a separate node from M132 - Native Lens Distortion's model-based LensDistortion.

Licensing: lensfun is LGPL-3 and its database is CC-BY-SA; record both in `tools/license/components/README.md`.

## Scope

**In:**
- The lensfun dependency and its bundled database.
- Lens metadata for raw files.
- The LensCorrect node (distortion, TCA, vignetting) on CPU and GPU, with metadata autofill and manual overrides.
- Tests and Xvfb screenshots.

**Out:**
- Nuke, 3DE and PanoTools models, STMap output and overscan: M132 - Native Lens Distortion.
- Feeding lensfun profiles into LensDistortion: none (separate nodes, by decision).
- Raw-domain (pre-demosaic) lens correction: none.

**Edge cases:**
- A lens or camera not in the database: the node passes through, with a persistent message naming what it looked up.
- Partial metadata (no focus distance, say): use lensfun's defaults and show which fields were inferred.
- Strong distortion: the sampler's Jacobian path keeps filtering correct.
- TCA: a coordinate map per channel.

**Assumptions:**
- rawspeed does not expose lens EXIF. The raw file's TIFF/EXIF IFDs have to be read separately (P1.T2 settles how).
- lensfun's database is current enough for typical bodies; updating it is routine maintenance.

## Phase 160.1: Dependency and metadata

- [ ] M160.P1.T1 — Add the lensfun dependency
  - files: `tools/ci/local/fetch-assets.sh`, `Engine/CMakeLists.txt`, `tools/license/components/README.md`, `tools/release/stage-bundle.sh` (database bundling)
  - approach: pin and build lensfun the way M80.P1.T1 builds rawspeed (the `LCMS2_REPO`/`LCMS2_REF` recipe), and bundle its database. Check the image for an existing lensfun first.
  - verify: a test binary opens the bundled database and finds a known lens entry; the licence README names lensfun and the database licence.
  - size: M

- [ ] M160.P1.T2 — Lens metadata for raw files
  - files: `Engine/Nodes/IO/RawDecodeSupport.cpp`, `Engine/Nodes/IO/RawMetadata.h/.cpp`, `Tests/Native/RawDecodeSupport_Test.cpp`
  - approach: fill `exif/Make`, `exif/Model`, `exif/LensModel`, `exif/FocalLength`, `exif/FNumber` and `exif/SubjectDistance` (the same key names `OiioReadSupport.cpp:633` gives non-raw files) for raw files from their EXIF IFDs. Scout first: rawspeed's TIFF parser, or a small pinned exiv2. Prefer whichever adds no new dependency.
  - verify: gtest: the Bayer and X-Trans fixtures from M80 - Native RAW Support carry the expected lens keys.
  - size: M

## Phase 160.2: LensCorrect node

- [ ] M160.P2.T1 — Distortion and TCA via the coordinate sampler
  - files: new `Engine/Nodes/Transform/LensCorrect.h/.cpp` + per-node files (`.reg.inc`, `.costseed.inc`, `docs/LensCorrect.md`, icon), new `Tests/Native/NativeLensCorrect_Test.cpp`
  - approach: port the geometry part of darktable `src/iop/lens.cc` at M80's `DARKTABLE_REF`. Build the inverse coordinate map from the lensfun modifier: one map, plus per-channel offsets for TCA. Sample through `resampleRowFromCoords` with the Jacobian. RoD and RoI come from the map with a bounded halo. Autofill reads `exif/*` and `raw/*`, and knobs override every field. An unknown lens passes through with a persistent message.
  - verify: gtest: a synthetic grid through a known profile lands within a sub-pixel bound; tiled equals full-frame; an unknown lens passes through with the message.
  - size: L

- [ ] M160.P2.T2 — Vignetting correction
  - files: `Engine/Nodes/Transform/LensCorrect.cpp`, `Tests/Native/NativeLensCorrect_Test.cpp`
  - approach: apply lensfun's vignetting model as a per-pixel radial gain in the same render, after sampling, behind its own enable knob.
  - verify: gtest: a synthetic vignetted flat field comes back flat within a bound.
  - size: M

- [ ] M160.P2.T3 — GPU path
  - files: new `Engine/Gpu/Kernels/lenscorrect.slang`, `lenscorrect.cmake`, `Engine/Nodes/Transform/LensCorrect.cpp`, new `Tests/Native/NativeLensCorrectGpu_Test.cpp`
  - approach: generate the coordinate map and vignetting gain in Slang (porting darktable's `lens.cl`), then sample through `coordmap.slang`.
  - verify: GPU parity against the CPU node on the grid and flat-field cases (README "GPU parity").
  - size: L

- [ ] M160.P2.T4 — Panel check under Xvfb
  - files: new `Tests/gui/m160_lenscorrect.py`
  - approach: open a raw fixture through Develop RAW and a JPEG with EXIF lens data, add LensCorrect, and confirm that the autofilled fields and a manual override take effect. Screenshot the panel and viewer, and share them with the user (project rule).
  - verify: the script passes under Xvfb; the user approves the screenshots.
  - size: M

**Verification gate:** `ctest` is green, including `NativeLensCorrect`, `NativeLensCorrectGpu` and the extended `RawDecodeSupport` cases; the Xvfb script passes and the user has approved the screenshots.
