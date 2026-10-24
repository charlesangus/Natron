# M80 - Native RAW Support

Brings RAW camera files back as a native pipeline ported from darktable. rawspeed decodes the file, and the native Read emits the undemosaiced sensor data on a new `eDataKindRaw` edge. Native nodes ported from darktable's CPU modules then turn it into a scene-linear image: rawprepare, temperature, highlights, demosaic and input colour, plus hot pixels, chromatic aberrations and raw denoise. A plain RawToImage converter and a viewer that shows raw directly let the user look at any stage. The core chain gets its GPU kernels here (Phase 80.6). The rest of the raw GPU work is in M81 - Raw GPU Kernels, and lens correction is its own milestone, M160 - Lensfun Lens Correction.

This supersedes the "RAW camera formats are dropped" clause of `DECISIONS/2026-10-09-native-read-format-scope.md`. The "different technique, not OIIO's libraw plugin" that clause anticipated is rawspeed plus these nodes. It also revises `DECISIONS/2026-10-09-raw-support-via-rawspeed-and-darktable-ports.md`; see `DECISIONS/2026-10-22-raw-support-grill-revisions.md`.

Licensing: darktable's modules are GPL-3.0-or-later, rawspeed is LGPL-2.1, and Natron is GPL-2-or-later. Ported files keep their darktable copyright headers, and the combined work is distributed under GPL-3 terms. Record this in `tools/license/components/README.md`.

Every node is built on the M137 - Native Base Class Flexibility framework (`NodeSpec`, `ImagePipelineSpec` op kinds, `StagePlan`). Each node adds the per-node files from M120 - Native Node Port Kit, following the Nodes README checklist "Adding a native node". Mosaic parameters travel as M74 - Native Metadata Core image metadata. The reader structure is M75 - Native Read's.

## Scope

**In:**
- rawspeed decode, pinned. Read emits raw kind for every RAW file: a 1-channel CFA mosaic, or 3 or 1 channels for linear DNGs and other files with no CFA.
- The `eDataKindRaw` data kind, with raw-kind nodes on the native framework.
- CPU ports of rawprepare, temperature, highlights, demosaic, input colour, hot pixels, CA correct and raw denoise, each with every user-facing control darktable `release-5.6.2` offers and darktable's defaults (2026-10-23 user decision).
- GPU kernels and parity tests for the core chain: the mosaic in GPU regions, Prepare, Temperature, Highlights, Demosaic PPG and RCD, and InputColor.
- A RawToImage node: a plain raw→image conversion with no processing.
- The Viewer accepts raw edges directly.
- Develop RAW is inserted automatically, as loose nodes inside a "Develop RAW" backdrop.
- A darktable reference harness, a large-image check, and Xvfb panels with screenshots.

**Out:**
- GPU for Markesteijn, AMaZE, VNG, hot pixels, CA, denoise and every mode added in Phase 80.7: M81 - Raw GPU Kernels.
- Lens correction and the lensfun dependency: M160 - Lensfun Lens Correction.
- Groups that carry non-image kinds: M159 - Kind-Agnostic Groups. Develop RAW stays a backdrop permanently, even after that lands.
- Any fallback decoder (OIIO's libraw plug-in, rawspeed's generic mode): none.
- darktable's SSE paths and ICC/DCP profile handling: none.
- Multi-frame raw (pixel shift, HDR bracket merges) and embedded-preview extraction: none.

**Edge cases:**
- Linear DNG, or a sensor with no CFA: raw kind with 3 or 1 channels and `raw/cfa_pattern` = `none`. Prepare, Temperature and Highlights handle any channel count. Demosaic passes the data through as image kind. Develop is still inserted.
- A camera the pinned rawspeed doesn't support: the Read shows a persistent error naming the make, model and rawspeed version, and renders nothing.
- A node fed the wrong `raw/stage` (Temperature on an unprepared mosaic, for example): a persistent error naming the node it needs upstream, and no render. Prepare on an already-prepared stream passes through with a warning instead.
- Develop RAW auto-insert fires only when the user creates a raw Read interactively (Tab menu, file drop or browser). It never fires on project load, Python `createNode`, or a filename change on an existing Read.
- Changing a Read's filename between RAW and non-RAW flips its output kind. Downstream edges go through the existing conflict reporting and are never disconnected.
- A crop or odd offset shifts the CFA pattern origin through `RawLayout`.

**Assumptions:**
- raw.pixls.us has small CC0 fixtures for a Bayer camera, an X-Trans camera and a linear DNG.
- darktable builds in a throwaway container for the reference harness. Docker Hub pulls in the sandbox may be blocked; if so, generate the references on a host that can pull.
- `GpuBuffer`/`GpuWrap` can carry a 1-channel region, or can be made to with bounded work (P6.T2 confirms this).

## Phase 80.1: Foundation

- [x] M80.P1.T1 — Vendor rawspeed as a pinned dependency and pin the darktable source release
  - files: `tools/ci/local/fetch-assets.sh`, `Engine/CMakeLists.txt`, `tools/license/components/README.md`, `tools/release/stage-bundle.sh` (only if the shared object needs bundling)
  - approach: build rawspeed from source at an exact commit SHA, plus pugixml if the image lacks it. Follow the `LCMS2_REPO`/`LCMS2_REF` recipe (`fetch-assets.sh:189-190`, clone at `:321`) and install into `build/assets/plugin-src/deps-install/`. Link it statically if rawspeed supports that, so the AppImage needs nothing new. `find_package` or pkg-config it in `Engine/CMakeLists.txt` and link it. Add the licence entries: rawspeed LGPL-2.1, pugixml MIT, and the darktable GPL-3 note. Also record one darktable **release tag** (the latest release) as `DARKTABLE_REF`. Every port in this milestone names it in its file header, and the reference harness builds it. Check the dev image first (`docker ps`, then what the image ships) before building anything. The bundle must include rawspeed's `cameras.xml`, which the decoder needs at run time. No build, cmake or Dockerfile references rawspeed, lensfun, pugixml or libraw today.
  - verify: a one-line test binary links rawspeed and constructs a `RawDecoder`; a second run of the fetch step is idempotent; the licence README names both libraries and the darktable tag.
  - size: M

- [x] M80.P1.T2 — Add the `eDataKindRaw` data kind
  - files: `Global/Enums.h`, `Engine/Node.cpp`, `Engine/Settings.{h,cpp}`, `Engine/OutputSchedulerThread.cpp`, `Engine/NodeColorSampler.cpp`, `Gui/NodeGui.cpp` (pen refresh only), `Engine/Nodes/README.md`
  - approach: add `eDataKindRaw` to `DataKindEnum` (`Global/Enums.h:515-520`, beside Image, Deep, Scene and Polymorphic). The payload is a float `Image` in the existing image cache: one channel for a mosaic, three or one for linear raw. There is no new render virtual or cache (unlike Deep's `EffectInstanceRenderDeep.cpp`); the kind exists for edge typing only. **Add no row to `isRegisteredAdapter`** (`Node.cpp:1892`). Conversions are explicit (README "Conversions between kinds are explicit"): Demosaic and RawToImage declare raw in and image out, and the Viewer exception is P2.T4. Edge colours are Preferences knobs: add an `_edgeRawColor` knob with an Okabe-Ito default distinct from Deep's blue, a `getEdgeRawColor`, and a Raw arm in `Settings::getEdgeKindColor` (`Engine/Settings.cpp:~3132`); `Edge::kindTintColor` already goes through it. Add raw arms to the kind switches in `Engine/OutputSchedulerThread.cpp` (~2505, ~2656) and `Engine/NodeColorSampler.cpp:175`. Document the kind in the README "Data kinds" section, including that the layout travels as metadata (P1.T3).
  - verify: gtest in `Tests/DataKind_Test.cpp`: a raw output cannot connect to an image-only input (`eCanConnectInput_incompatibleDataKind`) and can connect to a raw input; a project with a conflicting edge reports it on load through `Project::reportDataKindConflicts`; full ctest unchanged.
  - size: M

- [x] M80.P1.T3 — Define the raw metadata schema
  - files: new `Engine/Nodes/IO/RawMetadata.h/.cpp`, new `Tests/Native/RawMetadata_Test.cpp`
  - approach: typed accessors over `ImageMetadata` (`Engine/Nodes/Metadata/ImageMetadata.h`: `getString`/`getDoubleVector`/`setDoubleVector`, which return `std::optional`) for the keys every raw node needs:
    - `raw/cfa_pattern`: 2x2 Bayer, 6x6 X-Trans or `none`, as a string, plus the pattern origin offset.
    - `raw/black_levels` per CFA colour, `raw/white_level`.
    - `raw/wb_coeffs` (as shot).
    - `raw/cam_to_xyz`: a 3x3 D65 matrix from rawspeed's camera database.
    - `raw/crop` (active area), `raw/iso`, `raw/exposure`.
    - `raw/stage`: `raw`, or `prepared` (scaled to 0-1 with black subtracted).

    Provide `RawLayout` (pattern period, `colourAt(x, y)` with the crop offset applied, `hasCfa()`) so every node shares one definition. A node that crops or shifts the window must update the pattern origin; document that. Add a `requireStage(metadata, expected, nodeLabel)` helper returning the persistent-error text for a wrong stage.
  - verify: gtest round-trips every key. `colourAt` matches a hand table for RGGB, BGGR, GRBG, GBRG and a 6x6 X-Trans, including after a one-pixel crop offset. `hasCfa` is false for `none`. `requireStage` gives the expected text.
  - size: M

- [x] M80.P1.T4 — Host raw-kind nodes on the native framework
  - files: `Engine/Nodes/NodeSpec.h`, `Engine/Nodes/Image/ImagePipelineSpec.h`, `Engine/Nodes/Image/StagedImageEffect.h/.cpp`, `Engine/Nodes/README.md`, new `Tests/Native/RawKindFramework_Test.cpp`
  - approach: `StagedImageEffect` ops are image-kind today. Let an `ImagePipelineSpec`/`NodeSpec` declare raw in with raw out, or raw in with image out. Ops already see pixel position through `RowIO::x0`/`y` (`Engine/Nodes/Image/PixelKernel.h:69`), so a Point op can call `RawLayout::colourAt`. Stages that make no sense on a mosaic are rejected at compile time for raw-in specs: Premult, and the layer and channel selection. The framework checks `raw/stage` through `RawMetadata::requireStage` before rendering and raises the persistent error. Scout first: if raw-in specs need more than a kind field plus those restrictions, report it before implementing. Document raw nodes in the README "Flat image nodes" section.
  - verify: gtest with a test-only raw Point op: declared kinds resolve; a raw→raw op sees the right `colourAt` per row; a raw→image op connects to an image consumer; a wrong `raw/stage` sets the persistent message and renders nothing.
  - size: L

## Phase 80.2: Decode and read

- [x] M80.P2.T1 — rawspeed decode support
  - files: new `Engine/Nodes/IO/RawDecodeSupport.h/.cpp`, new `Tests/Native/RawDecodeSupport_Test.cpp`, new fixtures under `Tests/fixtures/raw/`
  - approach: model it on `OiioReadSupport`. A header cache keyed on (path, mtime, size) holds the parsed `RawImage` metadata behind a mutex. `decode(path, RectI window, float* dst, stride)` copies the window as float. rawspeed decodes whole files, so keep one decoded `RawImage` per cache key in a size-bounded LRU. The cap is a fraction of the project cache, not a constant, so it suits many-core high-RAM machines and modest ones alike. Fill `RawMetadata` from the decoder: CFA (`none` and the real channel count for linear DNGs), black and white levels, as-shot white balance, the camera matrix from `cameras.xml`, and the crop. A camera rawspeed rejects or marks unsupported fails with an error naming the make, model and rawspeed version. All offsets are `size_t`. Fixtures: small CC0 raw files from raw.pixls.us — a Bayer file, an X-Trans RAF and a linear DNG. Record their sources in `Tests/fixtures/raw/README.md`.
  - verify: gtest checks dimensions, CFA pattern, channel count, levels and crop against rawspeed's own output for each fixture. A window decode equals the same window of the full decode, bit for bit. A truncated file fails cleanly, and an unsupported-camera stub gives the named error.
  - size: M

- [x] M80.P2.T2 — Let the native Read resolve its output kind from the file
  - files: `Engine/Nodes/IO/NativeRead.cpp/.h`, `Engine/Nodes/README.md`, new `Tests/Native/NativeSourceKind_Test.cpp`
  - approach: the mechanism already exists. A node declared `eDataKindPolymorphic` is resolved through `NativeEffectBase::resolveOutputDataKind(bool*)` (`NativeEffectBase.h:253`), and a policy that reads a knob must call `Node::invalidateEffectiveOutputDataKindCache()` itself (`NativeEffectBase.h:~244`). Declare NativeRead's output polymorphic and override `resolveOutputDataKind` to return Raw when the filename's extension is in the RAW set (P2.T3) and Image otherwise, never ambiguous. Invalidate on `filename` changes. Downstream edges whose kind flips go through the existing conflict reporting and are never disconnected. A source node with a knob-driven kind is new, so document it in the README.
  - verify: gtest: a Read whose filename moves PNG→RAW→PNG flips Image→Raw→Image; a raw-only consumer reports a conflict when it flips back; a project save/load round trip keeps the resolved kind (`Tests/DataKindProjectLoad_Test.cpp` pattern).
  - size: M

- [x] M80.P2.T3 — Read renders raw for RAW files
  - files: `Engine/Nodes/IO/NativeRead.cpp`, `Engine/Nodes/IO/OiioReadSupport.cpp` (extension claim only), `Engine/Nodes/IO/docs/NativeRead.md`, new `Tests/Native/NativeReadRaw_Test.cpp`
  - approach: the native Read claims a constant set of rawspeed's supported extensions (cr2, cr3, nef, arw, raf, rw2, orf, dng, pef, srw, and the rest rawspeed lists) before `readableExtensions()` (`OiioReadSupport.h:302`). OIIO's `raw` stays in `kExcludedOiioFormats` (`OiioReadSupport.cpp:815`). For a RAW file the Read outputs `eDataKindRaw`: the decoded channels, the active-area window as the RoD, data from `RawDecodeSupport`, and the `RawMetadata` keys merged into the file metadata (`deriveOutputMetadata`). `ocioInputSpace` does not apply to raw data, so hide the colour knobs for a raw file. Frame sequences and the time domain work as for images. Proxy is disabled for raw, because downscaling a mosaic is meaningless; the tooltip says so. An unsupported camera sets a persistent error with P2.T1's text. Replace the "RAW is excluded; a later milestone adds it" line in `NativeRead.md:87`.
  - verify: gtest: a Read of each fixture is raw kind with the expected channel count, metadata keys and values; a non-RAW file still outputs an image; the unsupported-camera path sets the persistent error.
  - size: M

- [x] M80.P2.T4 — The Viewer accepts raw directly
  - files: `Engine/ViewerInstance.cpp/.h`, new `Engine/Nodes/Raw/RawDisplay.h/.cpp`, `Engine/Nodes/README.md`
  - approach: extend the Viewer's adapter exception (`ViewerInstance.h`, `inputAcceptsDataKindViaAdapter`, which returns `kind == eDataKindDeep` today) to Raw. `isRegisteredAdapter` (`Node.cpp:~1895`) hard-codes Deep→Image; make the Raw acceptance Viewer-only (no general Raw→Image adapter row, per P1.T2), choosing the smallest mechanism that keeps other image inputs rejecting raw. Display goes through a shared `RawDisplay` conversion: a mosaic becomes a greyscale single channel, and linear raw shows its channels as they are. RawToImage (P2.T5) reuses the same function. No demosaic in the viewer. The info line shows "RAW mosaic" or "RAW linear". Update the README's Viewer-exception paragraph.
  - verify: gtest: a viewer fed by a raw Read renders without error, and its pixel values equal the mosaic; a linear DNG shows three channels. Extend the `Tests/gui` smoke script to show it.
  - size: M

- [x] M80.P2.T5 — RawToImage node
  - files: new `Engine/Nodes/Raw/RawToImage.h/.cpp`, `RawToImage.reg.inc`, `docs/RawToImage.md`, new `Tests/Native/NativeRawToImage_Test.cpp`
  - approach: raw in, image out, no knobs beyond the framework's. A plain conversion through `RawDisplay` (P2.T4): mosaic → a 1-channel grey image (alpha 1), linear → channels as they are. No scaling, no colour. It is the explicit converter for users who want raw values in an image graph. Tab menu `Raw/RawToImage` via `NodeSpec::grouping`.
  - verify: gtest: output equals the input values for a mosaic and a linear fixture; output kind is image; `expectNativeIdentity` passes.
  - size: M

## Phase 80.3: Core develop chain (CPU)

Each port names the darktable source file in its `approach` as the reference. Ports take the C path at `DARKTABLE_REF` (P1.T1) and never the SSE or OpenCL variants; core-chain GPU kernels are Phase 80.6. Ports keep the darktable copyright header. Every node is a framework node (P1.T4) under `Engine/Nodes/Raw/`. Per node, it adds `<Node>.reg.inc`, `<Node>.costseed.inc`, `docs/<Node>.md`, an icon and a `Tests/Native/Native<Node>_Test.cpp` calling `expectNativeIdentity`, per the README checklist; no shared file is edited. Per-pixel mosaic work is a Point op that uses `RowIO::x0/y`. Neighbourhood work is a Passes op, or Custom where the algorithm's structure needs it. Each node declares raw in and raw out unless stated, and checks `raw/stage`. Prepare, Temperature and Highlights accept 1 or 3 channels (linear raw). Nodes sit in the Tab menu under `Raw` (`NodeSpec::grouping`).

- [x] M80.P3.T1 — RawPrepare node (levels, scaling, crop)
  - files: new `Engine/Nodes/Raw/RawPrepare.h/.cpp` + per-node files, new `Tests/Native/NativeRawPrepare_Test.cpp`
  - approach: port darktable `src/iop/rawprepare.c`. Subtract per-CFA-colour black levels (per channel for linear), scale by the white level to 0-1, apply the crop (shifting the pattern origin via `RawLayout`), and set `raw/stage` to `prepared`. A `prepared` input passes through with a warning. Knobs: black levels (override), white level (override), crop margins. Defaults come from the metadata.
  - verify: gtest against a hand-computed mosaic with known levels; a crop by an odd number of pixels keeps `colourAt` correct; a linear 3-channel input scales per channel; a prepared input passes through unchanged.
  - size: M

- [x] M80.P3.T2 — Temperature node (white balance on raw)
  - files: new `Engine/Nodes/Raw/RawTemperature.h/.cpp` + per-node files, new `Tests/Native/NativeRawTemperature_Test.cpp`
  - approach: port the raw-domain part of darktable `src/iop/temperature.c`: per-CFA-colour gains on the prepared mosaic, or per-channel gains on linear data. Knobs: temperature, tint, the three channel coefficients, and a mode (as shot, camera reference, custom). The temperature-to-coefficient conversion uses the camera matrix from metadata, as darktable does. Port the `dt_colorspaces_...` helpers it needs locally rather than pulling in darktable's colour library. It requires `raw/stage` = `prepared`.
  - verify: gtest: as-shot coefficients reproduce rawspeed's; a custom temperature on a synthetic grey patch with a known matrix gives equal channels after demosaic (checked once P3.T4 lands); an unprepared input raises the stage error.
  - size: M

- [x] M80.P3.T3 — Highlights node (clip and reconstruct)
  - files: new `Engine/Nodes/Raw/RawHighlights.h/.cpp` + per-node files, new `Tests/Native/NativeRawHighlights_Test.cpp`
  - approach: port darktable `src/iop/highlights.c`. Modes: clip, unclipped (leave), and reconstruction ("inpaint opposed" at `DARKTABLE_REF`; LCh where the pattern allows). The clip threshold needs the white level and white-balance coefficients from metadata.
  - verify: gtest: a synthetic mosaic with a clipped patch, each mode matching a reference computed from darktable on the same input. The reference is generated once by the P5.T1 harness and stored as a small fixture; the fixture README says how.
  - size: M

- [x] M80.P3.T4 — Demosaic node: PPG and RCD (Bayer), linear pass-through
  - files: new `Engine/Nodes/Raw/RawDemosaic.h/.cpp` + per-node files, new `Engine/Nodes/Raw/DemosaicBayer.h/.cpp`, new `Tests/Native/NativeRawDemosaicBayer_Test.cpp`
  - approach: port darktable `src/iop/demosaic.c` and `src/iop/demosaicing/ppg.c` and `rcd.c`. Raw in, image out: three colour channels plus alpha 1. Input with no CFA (linear) passes through as image with alpha 1. The halo comes from the algorithm (RCD needs 4-6 px), rounded up to the pattern period, and the RoD equals the input's. Knobs: method, green equilibration, colour smoothing passes. The method knob is an enum, so P3.T5 and P3.T6 add choices.
  - verify: gtest: a synthetic gradient mosaic reconstructs within a bound; a tile rendered with RoI expansion equals the same pixels of the full-frame render, bit for bit; output kind is image; a linear fixture passes through unchanged.
  - size: L

- [x] M80.P3.T5 — Demosaic: AMaZE
  - files: new `Engine/Nodes/Raw/DemosaicAmaze.h/.cpp`, `Engine/Nodes/Raw/RawDemosaic.cpp` (method enum), new `Tests/Native/RawDemosaicAmaze_Test.cpp`
  - approach: port darktable `src/iop/demosaicing/amaze.cc` (GPL-3). It works in 128-px tiles with its own overlap. Map that onto Natron's RoI (an 18 px halo, aligned to the pattern), not onto a whole-image buffer.
  - verify: gtest: tiled equals full-frame on a fixture; the result is within a bound of PPG's on a smooth patch and measurably sharper on an edge pattern.
  - size: L

- [x] M80.P3.T6 — Demosaic: X-Trans (Markesteijn)
  - files: new `Engine/Nodes/Raw/DemosaicXTrans.h/.cpp`, `Engine/Nodes/Raw/RawDemosaic.cpp`, new `Tests/Native/RawDemosaicXTrans_Test.cpp`
  - approach: port darktable `src/iop/demosaicing/xtrans.c`: 1-pass and 3-pass Markesteijn, plus the fast VNG variant for X-Trans previews. The 6x6 pattern period drives the halo alignment.
  - verify: gtest: on the X-Trans fixture, tiled equals full-frame; the output has no pattern artefacts on a flat patch.
  - size: L

- [x] M80.P3.T7 — Input colour node (camera to working space)
  - files: new `Engine/Nodes/Raw/RawInputColor.h/.cpp` + per-node files, `Engine/Project.cpp/.h` (only if a working-space accessor is missing), new `Tests/Native/NativeRawInputColor_Test.cpp`
  - approach: image in, image out, placed after demosaic: a Point op. Camera RGB goes to XYZ D65 through the metadata matrix (`raw/cam_to_xyz`), which the user can override; an ICC/DCP-less "standard matrix" is the fallback. Then XYZ goes to the project working space through the project OCIO (`DESIGN/2026-10-02-project-ocio.md`): build an OCIO matrix transform to the working space rather than hard-coding primaries. Port the matrix handling from darktable `src/iop/colorin.c`, not its ICC profile path.
  - verify: gtest: a grey-card mosaic with the metadata matrix ends neutral in the working space; changing the project working space re-renders.
  - size: M

- [x] M80.P3.T8 — Develop RAW: auto-insert the chain on interactive create
  - files: new `Engine/Nodes/Raw/RawDevelop.h/.cpp` (chain builder), the GUI node-creation hook found while scouting (Tab-menu create, file drop and file-browser create paths in `Gui/`), `Engine/Settings.cpp` (preference), `Tests/gui/m80_raw.py`
  - approach: when the user interactively creates a Read on a RAW file, insert RawPrepare → Temperature → Highlights → Demosaic → InputColor below it with darktable's defaults, as **loose nodes inside a backdrop** titled "Develop RAW". Groups are an image barrier (README "Known limitation"), and the backdrop is the permanent form. A "Develop RAW files on read" preference turns it off; it defaults on. It never fires on project load, Python `createNode`, or a filename change on an existing Read. Scout the creation paths first and put the hook at the single point they share; report it if no such point exists. The chain builder lives in Engine so tests can call it.
  - verify: gtest calls the chain builder on a fixture Read and checks the five nodes, their order and the backdrop. The Xvfb script creates a Read on a fixture through the GUI path and sees the chain and a plausible colour image in the viewer; loading a saved project does not add a second chain; ctest is otherwise unchanged.
  - size: M

## Phase 80.4: Extra modules

- [x] M80.P4.T1 — Hot pixels node
  - files: new `Engine/Nodes/Raw/RawHotPixels.h/.cpp` + per-node files, new `Tests/Native/NativeRawHotPixels_Test.cpp`
  - approach: port darktable `src/iop/hotpixels.c`: threshold and strength, running on the prepared mosaic with a 2-px halo (Passes).
  - verify: gtest: injected hot pixels on a smooth mosaic are removed, and a clean mosaic comes back bit-identical.
  - size: M

- [x] M80.P4.T2 — Chromatic aberration node
  - files: new `Engine/Nodes/Raw/RawCACorrect.h/.cpp` + per-node files, new `Tests/Native/NativeRawCACorrect_Test.cpp`
  - approach: port darktable `src/iop/cacorrect.c` (Bayer; X-Trans passes through with a message). It estimates shifts over the whole image, so it needs the full frame. Cache the shift estimate per (input hash, parameters) instead of recomputing it per tile, and have the RoI request the full input for estimation (Custom op).
  - verify: gtest: a synthetic mosaic with a known channel shift is corrected within a bound; tiled equals full-frame.
  - size: L

- [x] M80.P4.T3 — Raw denoise node
  - files: new `Engine/Nodes/Raw/RawDenoise.h/.cpp` + per-node files, bundled noise-profile data under `Engine/Nodes/Raw/data/`, new `Tests/Native/NativeRawDenoise_Test.cpp`
  - approach: port darktable `src/iop/rawdenoise.c` (wavelet). Ship profile data for the fixture cameras and read the rest from darktable's `noiseprofiles.json` at `DARKTABLE_REF`; record the pinned copy and its licence. Knobs: method, threshold or strength, ISO override.
  - verify: gtest: added Gaussian noise on a smooth mosaic is reduced measurably (PSNR improves), and edges are preserved within a bound.
  - size: L

- [ ] ~~M80.P4.T4 — Lens correction: add the lensfun dependency~~ (cancelled 2026-10-22: moved to M160 - Lensfun Lens Correction)
  - files: —
  - approach: —
  - verify: —
  - size: —

- [ ] ~~M80.P4.T5 — Lens correction node~~ (cancelled 2026-10-22: moved to M160 - Lensfun Lens Correction)
  - files: —
  - approach: —
  - verify: —
  - size: —

## Phase 80.5: Verification against darktable

- [x] M80.P5.T1 — Reference comparison harness
  - files: new `Tests/Native/RawReference_Test.cpp`, new `tools/raw/make-reference.sh`, `Tests/fixtures/raw/README.md`
  - approach: a script builds darktable at `DARKTABLE_REF` in a throwaway container. It develops each fixture with a fixed XMP sidecar (only the ported modules enabled, no tone mapping) to a 32-bit TIFF in the same working space, and the TIFF is committed as a small fixture. The test runs the Develop chain on the same file and compares with a tolerance per method (PPG and RCD close, AMaZE and Markesteijn within a looser bound), reporting mean and max error. If the sandbox can't pull the base image, generate the references on a host that can and commit them.
  - verify: the test passes for the Bayer and X-Trans fixtures; the README states the tolerances and why.
  - size: M

- [x] M80.P5.T2 — Large-image and threading check
  - files: new `Tests/Native/RawLarge_Test.cpp`, `tools/bench/`
  - approach: a synthetic 100 MP mosaic, generated at run time and never committed, runs through the whole chain. Check for no `int` overflow (`size_t` row arithmetic), memory bounded by the decode cache cap, and scaling across threads without a lock convoy (check with the `tools/bench` scripts, per the render-scaling findings). Concurrent renders of two views of one file share one decode.
  - verify: the gtest completes under a stated memory bound; the bench shows time falling with thread count.
  - size: M

- [ ] M80.P5.T3 — Panels and viewer check under Xvfb
  - files: `Tests/gui/m80_raw.py`, node panel layouts in `Engine/Nodes/Raw/*.cpp` (labels and pages only)
  - approach: run the Xvfb recipe (`build/deeprepro/run-gui.sh`, with `checkForUpdates` pre-seeded off). Open a Bayer, an X-Trans and a linear-DNG fixture, check the auto-inserted Develop backdrop, switch demosaic methods, change temperature, view a raw edge directly and through RawToImage. Capture screenshots of each node's panel and the viewer. Share the screenshots and get the user's approval before sign-off (project rule).
  - verify: the scripts pass under Xvfb; the screenshots are shared and the user approves them.
  - size: M

- [x] M80.P5.T4 — Viewer normalises raw for display; RawPrepare white-level knob fixes
  - files: `Engine/ViewerInstance.cpp`, `Engine/Nodes/Raw/RawDisplay.h/.cpp`, `Engine/Nodes/Raw/RawToImage.cpp` (only if it shares the scaled path), `Engine/Nodes/Raw/RawPrepare.cpp`, `Tests/Native/` viewer raw test, `Tests/gui/m80_raw.py`
  - approach: the Viewer shows a raw edge scaled to 0–1 for display only: subtract the black levels per CFA colour (or per channel) and divide by `raw/white_level` for a `raw`-stage stream; a `prepared` stream is already 0–1. RawToImage keeps raw sensor values unchanged (user decision). RawPrepare's White Level knob shows the file's value when Override is off, and its field is wide enough for 5 digits.
  - verify: gtest: a viewer on a raw Read shows values in 0–1 matching (value − black) / (white − black); RawToImage output still equals the input values. Xvfb: a raw Read at gain 0 shows a visible mosaic, not white.
  - size: M

## Phase 80.6: Core-chain GPU kernels

Kernels are Slang under `Engine/Gpu/Kernels/`, each with its own `<kernel>.cmake` fragment (`slang_add_kernel`). Point ops return `GpuOpRef::compiled` from `gpu()`, with the C++ maths mirrored in Slang and a `static_assert` on the parameter-block size (README "The GPU side of an op"). Passes ops use `PassList::gpu`. Ports start from darktable's OpenCL at `DARKTABLE_REF`. Parity follows README "GPU parity". CPU stays the default and the reference, and a machine with no usable device falls back silently.

- [ ] ~~M80.P6.T1 — Raw-kernel requirements for the engine GPU backend~~ (cancelled 2026-10-22: M83 - GPU Compute Backend Spike settled Vulkan compute + Slang, and core-chain kernels now ship in this phase)
  - files: —
  - approach: —
  - verify: —
  - size: —

- [x] M80.P6.T2 — Raw data in GPU regions
  - files: `Engine/Gpu/GpuBuffer.*`, `Engine/Gpu/GpuWrap.*`, `Engine/GpuPlacement.*`, `Engine/Nodes/Image/StagedImageEffect.cpp` (raw-in GPU path), new `Tests/Gpu/RawRegionGpu_Test.cpp`
  - approach: let a raw-kind region (1 or 3 channels) live resident on the GPU like an image region, and give kernels the `RawLayout` (pattern period, origin, the 6x6 colour table) as a uniform block. Placement treats raw-kind nodes as eligible only when every op in the run has a GPU path. Scout first whether `GpuBuffer`/`GpuWrap` already handle one channel; report if this needs more than bounded changes.
  - verify: a 1-channel mosaic uploads and reads back bit-exactly; a test kernel reading `colourAt` from the uniform matches the CPU `RawLayout` on every pixel of a Bayer and an X-Trans layout.
  - size: L

- [x] M80.P6.T3 — Prepare and Temperature kernels
  - files: new `Engine/Gpu/Kernels/rawprepare.slang`, `rawprepare.cmake`, `Engine/Nodes/Raw/RawPrepare.cpp`, `Engine/Nodes/Raw/RawTemperature.cpp`, new `Tests/Native/NativeRawPrepTempGpu_Test.cpp`
  - approach: one Slang module with two `IPixelOp`s (levels/scale/crop-offset, and per-CFA-colour gains), from darktable's `basic.cl` rawprepare and whitebalance kernels. Wire each node's `gpu()`.
  - verify: GPU parity against the CPU nodes on the Bayer, X-Trans and linear fixtures, within the README's parity tolerance.
  - size: M

- [x] M80.P6.T4 — Highlights kernels
  - files: new `Engine/Gpu/Kernels/rawhighlights.slang`, `rawhighlights.cmake`, `Engine/Nodes/Raw/RawHighlights.cpp`, new `Tests/Native/NativeRawHighlightsGpu_Test.cpp`
  - approach: port darktable's highlights OpenCL (clip, and the reconstruction mode P3.T3 ported). Clip is a Point kernel; reconstruction follows the CPU op's pass structure.
  - verify: GPU parity against the CPU node for each mode on the clipped-patch fixture.
  - size: M

- [x] M80.P6.T5 — Demosaic PPG and RCD kernels
  - files: new `Engine/Gpu/Kernels/demosaic_bayer.slang`, `demosaic_bayer.cmake`, `Engine/Nodes/Raw/RawDemosaic.cpp`, `Engine/Nodes/Raw/DemosaicBayer.cpp`, new `Tests/Native/NativeRawDemosaicGpu_Test.cpp`
  - approach: port darktable `demosaic_ppg.cl` and `demosaic_rcd.cl` (multi-pass, local memory) as `PassList::gpu` passes. AMaZE, Markesteijn and VNG report no GPU path, so a chain using them renders on the CPU (M81 - Raw GPU Kernels). Linear pass-through needs no kernel.
  - verify: GPU parity against the CPU node for PPG and RCD on the Bayer fixture; the tiled GPU render equals the full-frame GPU render.
  - size: L

- [x] M80.P6.T6 — InputColor kernel and resident-chain check
  - files: new `Engine/Gpu/Kernels/rawinputcolor.slang`, `rawinputcolor.cmake`, `Engine/Nodes/Raw/RawInputColor.cpp`, new `Tests/Native/NativeRawChainGpu_Test.cpp`
  - approach: a 3x3 camera→working-space matrix Point kernel (the OCIO matrix baked to the node's parameter block). Then check that the whole Develop chain, Prepare through InputColor with RCD, runs as one resident GPU region with no readback between nodes.
  - verify: GPU parity for InputColor; the chain test shows the end-to-end GPU result within tolerance of the CPU chain and a single upload/readback (from the placement trace).
  - size: M

## Phase 80.7: Full darktable control parity

> **Deferred (user, 2026-10-23):** darkroom-only display aids — the highlights mask views (clipped, candidates, combine, strength), the dual-demosaic mask, the capture-sharpen sharpened-areas and radius views, and hot-pixel Mark Fixed — are not built in this phase. They wait for a rich viewer-overlay milestone (requested via the inbox). Implementers skip the "Show Mask"/view parts of T8, T10, T21 and T23; T24 is cancelled; the T28 audit marks those rows DEFERRED with that reason, not MISSING.

This phase gives each ported module every user-facing control darktable `release-5.6.2` has, at darktable's defaults, including the defaults darktable takes from the file.

New knobs and modes, by module:
- **RawPrepare:** Flat Field (disabled / embedded GainMap). The default is embedded when the DNG carries four Bayer GainMaps.
- **RawTemperature:**
  - Two new modes: "From Image Area" (spot) and "As Shot to Reference".
  - The default mode becomes As Shot to Reference, darktable's default under its scene-referred workflow (T36).
  - A camera preset list from `wb_presets.json`, with Finetune.
  - The as-shot fallback chain for files that record no as-shot white balance.
- **RawHighlights:**
  - Three new modes: Segmentation Based, Guided Laplacians (Bayer only) and Reconstruct Color.
  - New knobs: Candidating, Combine, Rebuild (7 recovery modes), Strength, Noise Level, Iterations, Diameter of Reconstruction (scales), Inpaint a Flat Color.
  - The clipping threshold defaults to min(1, DNG LinearResponseLimit).
  - darktable's mode availability rules per sensor.
  - Mask views: clipped, candidates, combine, strength.
- **RawDemosaic:**
  - Bayer modes: LMMSE (+ LMMSE Refine), RCD (dual), AMaZE (dual), Passthrough (monochrome), Photosite Color (debug).
  - X-Trans modes: Frequency Domain Chroma, Markesteijn 3-pass (dual), Passthrough (monochrome), Photosite Color (debug).
  - Dual Threshold, with a dual-mask view.
  - Capture Sharpen: Enabled, Iterations, Radius (0 = per-image auto), Contrast Sensitivity (default from the image's noise), Corner Boost, Sharp Center. Two views: sharpened areas and overall radius.
- **RawInputColor:** the knob becomes darktable's input-profile list:
  - Matrices: Embedded Matrix (DNG, the DNG default), Standard Color Matrix (the default for other raws; this is today's "From File"), Enhanced, Vendor and Alternate matrices, wherever darktable has an entry for the camera.
  - Generic profiles: sRGB, Adobe RGB (compatible), linear Rec709 (today's misnamed "Standard (Linear Rec. 709)"), Rec709, linear Rec2020, PQ/HLG Rec2020, PQ/HLG P3, Display P3, linear ProPhoto, linear XYZ, Lab, linear infrared BGR, BRG (testing).
  - Gamut Clipping (off / sRGB / Adobe RGB / linear Rec709 / linear Rec2020).
  - Working Profile (user call 4).
  - The as-shot-to-reference late correction.
- **RawHotPixels:** Mark Fixed Pixels.
- **RawColorCalibration (new node, darktable's color calibration `channelmixerrgb`):** every control of its params v3 and GUI: CAT (adaptation, illuminant with F/LED sources, temperature, hue/chroma, gamut compression, clip), the R/G/B/colorfulness/brightness/gray mixing rows with normalize, the saturation algorithm, area color mapping, color-checker calibration and the built-in presets. Defaults come from the file. Develop RAW inserts it after RawInputColor (T29–T37).
- **RawCACorrect and RawDenoise:** already complete. I checked them against the code: denoise's curve x positions are fixed in darktable, so the 4x5 strength knobs cover them.

Not ported, as N/A rows in the audit:
- ICC/DCP `filename` and `filename_work`.
- colorin `intent` and `blue_mapping`: no widget in 5.6.2.
- 4-colour sensors: temperature `various`, Bayer4 demosaic methods (user call 5).
- channelmixerrgb illuminant detection from surfaces/edges: compiled out in 5.6.2 (`// #define AI_ACTIVATED`, channelmixerrgb.c:36). Its cosmetic GUI (illuminant and spot swatches, slider gradients, CCT label) is N/A too; the CCT goes in a tooltip.
- Deferred with the display aids above: RawColorCalibration's checker patch-grid and ΔE overlay (`gui_post_expose`); the audit marks it DEFERRED.

GPU kernels for every new mode go to M81: each new mode returns false from the node's `canRenderOpOnGpu`, as AMaZE does. The exception is RawColorCalibration: it joins the default Develop chain, so its kernel is built here (T35, user 2026-10-23).

The tasks below are serial within a node, because they share its `.cpp`. Separate nodes can run in parallel lanes, and every task adds its own test file rather than sharing one.

- [x] M80.P7.T1 — Extend the reference harness to every mode darktable-cli can render
  - files: `tools/raw/make-reference.py`, `tools/raw/make-reference.sh`, `Tests/fixtures/raw/README.md`
  - approach:
    - Add jobs, packing each params struct at the version in its module's `DT_MODULE_INTROSPECTION`:
      - Camera files, 192 px crops of the full-frame develop:
        - demosaic: VNG4, LMMSE × 5 refine levels, RCD dual and AMaZE dual (dual_thrs 0.2 and 0.6), PPG with median_thrs > 0, green equilibration (local/full/both), colour smoothing (1 and 5), Bayer passthrough mono and photosite colour.
        - X-Trans: VNG, FDC, Markesteijn 3-pass dual, passthrough mono and photosite colour.
        - Capture sharpening at a fixed radius and threshold, plus one auto run (cs_radius 0). For that run, capture darktable's `-d pipe` "bayer/xtrans autoradius" line into the README.
        - Temperature: as shot, camera reference, as shot to reference (preset 4, which exercises colorin's late correction), user coefficients.
        - Hotpixels: default, permissive, and strength 1.
        - CA correct: iterations 1/2/5 and avoidshift.
        - Raw denoise: threshold 0.05, flat curves and one shaped curve.
        - colorin: standard, enhanced/vendor/alternate where `src/common/colormatrices.c` lists the 5D III or X-Pro2, every generic profile, and gamut clipping × 4. Run the embedded matrix on the linear DNG.
      - Synthetic mosaics, extending `write_cfa_dng`, photosite-colour debug demosaic:
        - highlights segmentation (rebuild off, generic, small segments), guided laplacians (defaults and iterations 64 / noise 0.1 / solid colour 0.5) and reconstruct color, on the existing clipped-disc DNGs.
        - A Bayer DNG with an OpcodeList2 GainMap per CFA cell (flat field on and off).
        - A DNG with `LinearResponseLimit` 0.8 (clip default).
        - A mosaic with injected hot pixels.
    - Spot white balance and the wb_presets entries can't be rendered headless. darktable stores the picked coefficients, so those are compared in their own tasks.
    - Also correct the README's claim that darktable's default input profile is "enhanced matrix". `colorin.c` `reload_defaults` (~1904) picks the embedded matrix for a DNG and the standard matrix for other raws.
  - verify:
    - `python3 -I -m py_compile`.
    - A dry run lists every job and writes every XMP.
    - The README table lists each new reference with its tolerance class.
  - size: L

- [x] M80.P7.T2 — Regenerate all darktable references in the container, once
  - files: `Tests/fixtures/raw/reference/*` (new TIFFs and DNGs, Git LFS), `Tests/fixtures/raw/README.md`
  - approach:
    - Run `tools/raw/make-reference.sh` once for every job from T1 and T29. Record T29's "[commit color calibration]" lines in the README.
    - Check `docker ps` first. If the Docker Hub pull fails, generate on a host that can pull (per the milestone's assumption).
    - Commit through LFS, upload the LFS objects, and record the darktable autoradius line and every file's SHA-256 in the README.
  - verify:
    - Every job in the README table has a file.
    - Re-running one job reproduces it bit for bit.
    - The existing `RawReferenceTest` and `NativeRawHighlightsTest.MatchesTheDarktableReference` still pass against the regenerated files. Compiled at P7.T18.
  - size: M

- [x] M80.P7.T3 — Parity-audit tool and mapping
  - files: new `tools/raw/parity-audit.py`, new `tools/raw/darktable-parity.json`
  - approach:
    - The script parses, at `DARKTABLE_REF` (`/tmp/dt-src-562` or `build/darktable-src`):
      - every `dt_iop_*_params_t` field and enum `$DESCRIPTION` value of rawprepare, temperature, highlights, demosaic, colorin, hotpixels, cacorrect, rawdenoise and channelmixerrgb (its enums in `src/common/illuminants.h` and `src/common/chromatic_adaptation.h` too);
      - colorin's input-profile list (`colorspaces.c` entries with `++in_pos`, plus the image-profile types in `update_profile_list`);
      - a hand list of GUI-only controls that change the darkroom output (highlights mask modes, dual mask, capture-sharpen masks, temperature presets, finetune and spot; color calibration's area mapping mode, take-mixing toggle, target lightness/hue/chroma and picker; its checker chart, optimize-for, patch scale, calibrate, validate and patch overlay; its built-in presets).
    - The JSON maps each item to a node knob or choice option with darktable's default, or to N/A with a reason (ICC file, no widget, 4-colour sensor, compiled out (`AI_ACTIVATED`), cosmetic GUI), or to DEFERRED (display aid).
    - The script prints a markdown table (darktable item | Natron knob/option | default | status). It exits non-zero on any MISSING row, or on a knob name it can't find as a `#define kRaw*Param`/`setName` in `Engine/Nodes/Raw`.
    - Seed the JSON with today's state; the gaps listed above show as MISSING. Implementation tasks don't edit the JSON; T28 does.
  - verify: the script runs and reproduces the 2026-10-23 audit's gaps as MISSING rows, plus the mask-view rows the audit missed. Python only; no build.
  - size: M

- [ ] M80.P7.T4 — darktable comparisons for already-ported modes that have no comparison yet
  - files: new `Tests/Native/RawReferenceModules_Test.cpp`, `Tests/Native/RawReference_Test.cpp` (shared helpers only, moved into a header if needed), new `Tests/Native/RawReferenceSupport.h`
  - approach:
    - Compare these modes on the T2 crops, reusing `RawReference_Test`'s crop/convert/tolerance code:
      - VNG4, X-Trans VNG, PPG with edge threshold, green equilibration × 3, colour smoothing 1/5;
      - Temperature as shot, camera reference and custom;
      - hot pixels (default, permissive);
      - CA correct (iterations, avoid shift);
      - raw denoise (threshold, shaped curve).
    - Tolerances follow the README classes.
    - If a comparison fails and the fix is confined to that node's own file, fix it here. Otherwise report the mode, error and suspect function to the PM as a new task.
  - verify: the new gtests pass, with mean/max error reported per mode. Compiled at P7.T18.
  - size: L

- [x] M80.P7.T5 — Raw metadata for file-derived defaults: make/model, LinearResponseLimit, DNG embedded matrix
  - files: `Engine/Nodes/IO/RawMetadata.h/.cpp`, `Engine/Nodes/IO/RawDecodeSupport.cpp`, `Tests/Native/RawMetadata_Test.cpp`, `Tests/Native/RawDecodeSupport_Test.cpp`
  - approach:
    - Add these keys:
      - `raw/make` and `raw/model`: rawspeed's canonical names, the strings darktable's preset and matrix tables match on;
      - `raw/linear_response_limit` (default 1);
      - `raw/dng_d65_matrix`.
    - Read the tags through the decoder's `getRootIFD()`, as `exposureTime()` does (`TiffTag::LINEARRESPONSELIMIT`, `COLORMATRIX1/2/3`, `FORWARDMATRIX1/2/3`, `CALIBRATIONILLUMINANT1/2/3`, `ASSHOTNEUTRAL`). No exiv2.
    - Port the illuminant/ForwardMatrix to D65 logic from darktable `src/common/exif.cc` (~2065–2270, `illuminant_data`, `mat3mul` into `d65_color_matrix`), and `_check_linear_response_limit` (~914).
  - verify:
    - gtest: the linear DNG fixture yields the matrix darktable logs (`-d imageio` from the T2 run, recorded in the README).
    - CR2/RAF have no DNG matrix, and their LRL is 1.
    - The synthetic LRL DNG yields 0.8.
    - make/model match the wb_presets.json and colormatrices.c spellings for both cameras.
    - Compiled at P7.T18.
  - size: M

- [x] M80.P7.T6 — Parse DNG GainMap opcodes into metadata
  - files: new `Engine/Nodes/IO/RawDngOpcodes.h/.cpp`, `Engine/Nodes/IO/RawDecodeSupport.cpp`, `Engine/Nodes/IO/RawMetadata.h/.cpp`, new `Tests/Native/RawDngOpcodes_Test.cpp`
  - approach:
    - Port darktable `src/common/dng_opcode.c` `dt_dng_opcode_process_opcode_list_2` (GainMap, opcode 9; big-endian `_get_double`/`_get_float`). rawspeed returns `GainMap → nullptr` and never applies it (`DngOpcodes.cpp:778`), so Natron parses the OPCODELIST2 bytes itself.
    - Ignore OpcodeList3 (WarpRectilinear is lens correction, M160).
    - Store the maps as `raw/gain_maps`: per map, the region, plane, pitch, spacing, origin and points as a double vector.
    - A GainMap set too large for metadata is the caller's problem; real maps are ≤ ~100×100×4 floats.
  - verify: gtest parses the T2 synthetic GainMap DNG (four maps, values and geometry exact). A DNG with no opcodes yields no key, and a truncated opcode list fails cleanly. Compiled at P7.T18.
  - size: M

- [x] M80.P7.T7 — RawPrepare: flat-field correction from embedded GainMaps
  - files: `Engine/Nodes/Raw/RawPrepare.cpp/.h`, `Engine/Nodes/Raw/docs/RawPrepare.md`, new `Tests/Native/NativeRawPrepareFlatField_Test.cpp`
  - approach:
    - Add a Flat Field choice: Disabled / Embedded GainMap. The default is Embedded GainMap, which is darktable's file-derived default: on only when `_check_gain_maps` accepts the maps.
    - With no acceptable maps it does nothing, and the tooltip and a node message say so, as darktable hides the control.
    - Port `_check_gain_maps` (rawprepare.c:626, four Bayer maps of the same shape) and the gain-map branch of `process` (rawprepare.c:~404–440, bilinear map lookup per photosite) into the Point op. The maps' origin follows the crop through `RawLayout`.
    - Flat Field on reports no GPU path (M81).
  - verify: gtest plus the darktable comparison on the GainMap DNG, on and off. A crop by an odd pixel count still matches. A file without maps is unchanged with the knob on. Compiled at P7.T18.
  - size: M

- [x] M80.P7.T8 — RawHighlights: LinearResponseLimit clip default, per-sensor mode rules, clipped-mask view
  - files: `Engine/Nodes/Raw/RawHighlights.cpp/.h`, `Engine/Nodes/Raw/docs/RawHighlights.md`, new `Tests/Native/NativeRawHighlightsDefaults_Test.cpp`
  - approach:
    - The clipping threshold shows min(1, `raw/linear_response_limit`) until overridden. This follows RawPrepare's override pattern from P5.T4 (highlights.c:1255 `reload_defaults`).
    - Port the mode sanitising from highlights.c:1144–1158: Laplacian is Bayer only, and LCh/Reconstruct Color/Segmentation need a CFA. Each falls back to Inpaint Opposed with darktable's message as a node warning.
    - Add a "Show Mask" choice (Off / Clipped, extended by T9–T11), porting `process_visualize` (highlights.c:759) for the clipped view.
    - Keep Natron's "Unclipped" option; it maps to "module off" and is marked EXTRA in the audit.
  - verify: gtest checks the LRL DNG default is 0.8 and the CR2's is 1, plus the fallback per sensor type. The darktable comparison passes on the LRL DNG, and the clipped mask matches a hand-computed mask. Compiled at P7.T18.
  - size: M

- [x] M80.P7.T9 — RawHighlights: Reconstruct Color mode
  - files: new `Engine/Nodes/Raw/RawHighlightsInpaint.cpp`, `Engine/Nodes/Raw/RawKernels.h`, `Engine/Nodes/Raw/RawHighlights.cpp`, new `Tests/Native/NativeRawHighlightsInpaint_Test.cpp`
  - approach:
    - Port darktable `src/iop/hlreconstruct/inpaint.c` (`interpolate_color`, `interpolate_color_xtrans`, `interp_pix_xtrans`): row and column scans in four directions over the whole frame.
    - Render it as a whole-frame Custom op cached under `RawNodeSupport::frameKey()`, like Inpaint Opposed. The buffer is one float per photosite; that is 400 MB at 100 MP, which T25 checks.
    - No GPU path.
  - verify: gtest checks tiled = full frame bit for bit, plus the darktable comparison on the Bayer and X-Trans clipped-disc DNGs. Compiled at P7.T18.
  - size: M

- [x] M80.P7.T10 — RawHighlights: Segmentation Based mode
  - files: new `Engine/Nodes/Raw/RawHighlightsSegments.cpp`, new `Engine/Nodes/Raw/RawSegmentation.h/.cpp`, `Engine/Nodes/Raw/RawHighlights.cpp/.h`, new `Tests/Native/NativeRawHighlightsSegments_Test.cpp`
  - approach:
    - Port darktable `hlreconstruct/segbased.c` (`_process_segmentation`, `_calc_plane_candidates`, `_segment_gradients`, `_add_poisson_noise`, `_masks_extend_border`) and `hlreconstruct/segmentation.c` (`dt_segmentize_plane`, `dt_segments_combine`, `_floodfill_segmentize`, dilate/erode).
    - Knobs at darktable's defaults: Candidating 0.4, Combine 2.0, Rebuild (7 recovery modes, default off), Strength 0 (shown when Rebuild is on), Noise Level 0 (shown when Rebuild is on).
    - Reuse Opposed's whole-frame chroma (highlights.c:364 `use_opposing`).
    - The whole-frame result is cached per `frameKey()`. Plane buffers are at darktable's per-colour plane size, so memory scales with the frame. Use `size_t` offsets.
    - Extend Show Mask with Combine, Candidates and Strength.
    - No GPU path.
  - verify: gtest checks tiled = full frame, plus the darktable comparison on both synthetic DNGs for rebuild off, generic and small segments. The masks are non-empty exactly on the clipped disc. Compiled at P7.T18.
  - size: L

- [x] M80.P7.T11 — RawHighlights: Guided Laplacians mode (Bayer)
  - files: new `Engine/Nodes/Raw/RawHighlightsLaplacian.cpp`, `Engine/Nodes/Raw/RawKernels.h`, `Engine/Nodes/Raw/RawHighlights.cpp/.h`, new `Tests/Native/NativeRawHighlightsLaplacian_Test.cpp`
  - approach:
    - Port darktable `hlreconstruct/laplacian.c` `process_laplacian_bayer`, `_interpolate_and_mask`, `guide_laplacians`, `heat_PDE_diffusion`, `wavelets_process` and `_remosaic_and_replace`.
    - Knobs: Noise Level 0, Iterations 30, Diameter of Reconstruction (12 scale options, default 7 = WAVELETS_7_SCALE), Inpaint a Flat Color 0. All are hidden unless the mode is Laplacian.
    - The wavelet diffusion runs on the DS_FACTOR-downsampled frame. Cache that downsampled reconstruction per `frameKey()`, so memory is about 1/16 of the frame × 4 channels. The full-resolution interpolate, upsample and remosaic stays per tile, so tiled output is exact.
    - The `salt` noise must be deterministic per photosite, so tiles agree.
    - No GPU path.
  - verify: gtest checks tiled = full frame, plus the darktable comparison on the Bayer DNG (defaults and the iterations/noise/solid-colour variant). X-Trans falls back to Opposed per T8. Compiled at P7.T18.
  - size: L

- [x] M80.P7.T12 — RawTemperature: camera presets and finetune, with darktable's as-shot fallback
  - files: `Engine/Nodes/Raw/RawTemperature.cpp/.h`, new `Engine/Nodes/Raw/RawWbPresets.h/.cpp`, new `Engine/Nodes/Raw/data/wb_presets.json` (from `DARKTABLE_REF`), `Engine/CMakeLists.txt` (install to `Resources/darktable/` like `cameras.xml`), new `Tests/Native/NativeRawTemperaturePresets_Test.cpp`
  - approach:
    - Port `src/common/wb_presets.c` (load, lookup by `raw/make`/`raw/model`, `dt_wb_preset_interpolate`).
    - Add a "Camera Preset" mode with a Preset choice, repopulated from the input's make and model and saved by preset name, and a Finetune int. The Finetune range is the preset's min/max tuning; for a value between tunings, interpolate the way `_preset_tune_callback` (temperature.c:1782–1890) does.
    - Port temperature.c's missing-as-shot fallback (~1549 onward: Daylight preset, then D65 coefficients) into As Shot.
    - Record the data file and its licence in `tools/license/components/README.md`. Mention that edit in the PR, since it is a sixth file.
  - verify: gtest checks the 5D III "Daylight" coefficients equal wb_presets.json, and a between-tunings finetune equals a hand interpolation. The X-Pro2 lists its 7 presets. A camera without presets shows none. The "user coefficients" darktable comparison passes. Compiled at P7.T18.
  - size: L

- [x] M80.P7.T13 — RawTemperature: From Image Area (spot) mode
  - files: `Engine/Nodes/Raw/RawTemperature.cpp/.h`, `Engine/Nodes/Raw/docs/RawTemperature.md`, new `Tests/Native/NativeRawTemperatureSpot_Test.cpp`
  - approach:
    - Add Spot Area bottom-left/size knobs, drawn with the host rectangle interact (`node->addRectangleInteract`, as `Engine/Nodes/Image/ExtentKnobs.cpp:276` does), and a "Pick" button.
    - The button reads the source over the area at full scale, following Grade's `normalize()` pattern (`Grade.cpp:~315`). It averages per CFA colour, porting `_color_picker_bayer`/`_color_picker_xtrans` from `src/common/color_picker.c`.
    - It then writes the coefficients the way `color_picker_apply` does (temperature.c:1948: 1/mean normalised to green, clamped 0–8) into red/green/blue, and sets the mode to From Image Area.
    - Snapshot semantics, as darktable stores them; user call 2. The default area is darktable's default picker box, checked in `gui/color_picker_proxy.c`.
  - verify: gtest on a synthetic mosaic with known per-colour means gives the expected coefficients for Bayer and X-Trans. The render then equals Custom with the same coefficients. There is no darktable render, because darktable-cli can't pick. Compiled at P7.T18.
  - size: M

- [x] M80.P7.T14 — RawTemperature "As Shot to Reference" and RawInputColor's late correction
  - files: `Engine/Nodes/Raw/RawTemperature.cpp`, `Engine/Nodes/IO/RawMetadata.h/.cpp` (`raw/wb_late_correction`, `raw/wb_d65_coeffs`), `Engine/Nodes/Raw/RawInputColor.cpp`, new `Tests/Native/NativeRawLateCorrection_Test.cpp`
  - approach:
    - The new mode applies the as-shot coefficients, as darktable does (`_publish_chroma`, temperature.c:515–530). It publishes D65coeffs/as_shot per channel as `raw/wb_late_correction`, which travels through Highlights and Demosaic metadata. In every mode, RawTemperature also publishes darktable's D65 coefficients (`chr->D65coeffs`) as `raw/wb_d65_coeffs`. RawColorCalibration's `_get_white_balance_coeff` port needs them (T32).
    - RawInputColor multiplies by it before the matrix, except for Lab (colorin.c:1198–1210, `process_cmatrix` coefficients).
    - The default changes to As Shot to Reference in T36 (user, 2026-10-23; supersedes user call 1).
    - InputColor's GPU kernel gets the coefficients in its parameter block, or reports no GPU path while they are set. Prefer the parameter block if it needs no kernel change beyond a multiply; otherwise M81.
  - verify: gtest checks that As Shot to Reference into InputColor equals Camera Reference into InputColor within float error, and that `raw/wb_d65_coeffs` is present in every mode and equals the Camera Reference coefficients, plus the darktable comparison of the preset-4 render. Compiled at P7.T18.
  - size: M

- [x] M80.P7.T15 — RawInputColor: darktable's matrix profile types and file-derived default
  - files: `Engine/Nodes/Raw/RawInputColor.cpp/.h`, new `Engine/Nodes/Raw/RawColorMatrices.h/.cpp` (tables from `src/common/colormatrices.c`), `Engine/Nodes/Raw/RawColorMath.cpp/.h`, new `Tests/Native/NativeRawInputColorProfiles_Test.cpp`
  - approach:
    - Replace the mode choice with an Input Profile choice.
    - Matrix options:
      - Embedded Matrix (`raw/dng_d65_matrix`);
      - Standard Color Matrix (today's "From File", `raw/cam_to_xyz`);
      - Enhanced, Vendor and Alternate Matrix, from `dt_profiled_colormatrices`, `dt_vendor_colormatrices` and `dt_alternate_colormatrices` matched on make/model.
    - Port the primaries/white-to-matrix maths of `dt_colorspaces_create_darktable_profile`, `create_vendor_profile` and `create_alternate_profile` (`src/common/colorspaces.c`), with lcms's Bradford D50 adaptation. Then go to Natron's XYZ D65, consistent with the existing matrix path that already matches darktable to 0.013%.
    - Options a file lacks report "not available for this camera" and fall back as darktable's list does.
    - The default is darktable's `reload_defaults` (colorin.c:1904–1907): embedded for a DNG with a matrix, standard otherwise. Keep Natron's Custom matrix as EXTRA.
  - verify: gtest checks the default per fixture (CR2 → Standard, linear DNG → Embedded), plus the darktable comparisons for every matrix type T2 rendered. Compiled at P7.T18.
  - size: L

- [x] M80.P7.T16 — RawInputColor: generic input profiles
  - files: `Engine/Nodes/Raw/RawInputColor.cpp`, new `Engine/Nodes/Raw/RawColorProfiles.h/.cpp`, `Engine/Nodes/Raw/docs/RawInputColor.md`, new `Tests/Native/NativeRawInputColorGeneric_Test.cpp`
  - approach:
    - Add every `colorspaces.c` built-in with `++in_pos` that isn't a file: sRGB, Adobe RGB (compatible), linear Rec709 (renames "Standard (Linear Rec. 709)"), Rec709, linear Rec2020, PQ/HLG Rec2020, PQ/HLG P3, Display P3, linear ProPhoto, linear XYZ, Lab, linear infrared BGR, BRG.
    - Each is a linearising TRC plus a matrix. darktable runs these through `process_cmatrix` with its TRC LUTs and unbounded extrapolation; mirror that, not a clamp.
    - Lab converts Lab to XYZ D50 to working space.
    - The non-linear options report no GPU path.
  - verify: gtest round-trips each profile against an analytic reference, plus the darktable comparisons for each generic profile. Compiled at P7.T18.
  - size: M

- [x] M80.P7.T17 — RawInputColor: gamut clipping and working profile
  - files: `Engine/Nodes/Raw/RawInputColor.cpp/.h`, `Engine/Nodes/Raw/RawColorProfiles.cpp`, new `Tests/Native/NativeRawInputColorClipping_Test.cpp`
  - approach:
    - Gamut Clipping: off / sRGB / Adobe RGB / linear Rec709 / linear Rec2020, default off. Port `process_cmatrix_proper` with darktable's `nmatrix`/`lmatrix` clip to the chosen RGB gamut (colorin.c:1026).
    - Working Profile, if the user approves call 4: the default is "Project working space", plus darktable's `work_pos` profiles. A non-project choice converts the output there and says so in the node info.
    - Either path reports no GPU path when on.
  - verify: gtest checks out-of-gamut values clip to the chosen gamut and in-gamut values are untouched, plus the darktable comparisons for the 4 clipping options. Compiled at P7.T18.
  - size: M

- [x] M80.P7.T18 — Build checkpoint A
  - files: (none new; fixes go to the owning task's files)
  - approach:
    - Run `tools/ci/local/syntax-check.py --root .worktrees/m80` on the changed TUs.
    - One remote `tools/ci/remote/build.sh debug --tests` (PR-CI asserts) and one release gate, covering T4–T17.
    - Re-run failed cases with `-V`, and route each fix to its task.
  - verify: ctest is green on both legs, including every new `Tests/Native/*Raw*` test, the T4 comparisons and the updated `NativeGpuCapabilityTest`. T4–T17 get checked off.
  - size: M

- [x] M80.P7.T19 — RawDemosaic: passthrough (monochrome) and photosite colour, Bayer and X-Trans
  - files: `Engine/Nodes/Raw/RawDemosaic.cpp/.h`, `Engine/Nodes/Raw/RawDemosaicBasics.cpp`, `Engine/Nodes/Raw/RawKernels.h`, new `Tests/Native/NativeRawDemosaicPassthrough_Test.cpp`
  - approach:
    - Port `demosaicing/passthrough.c` (`passthrough_monochrome`, `passthrough_color`) as Point ops. Add the options to both method knobs.
    - The existing `passthroughMonochrome` kernel may already be darktable's; reuse it if so.
    - darktable's own "Monochrome" (true mono) is today's no-CFA pass-through and is mapped in the audit.
  - verify: gtest on hand tables, plus the darktable comparisons for Bayer and X-Trans passthrough mono and photosite colour. Compiled at P7.T26.
  - size: M

- [ ] M80.P7.T20 — RawDemosaic: LMMSE (Bayer) with LMMSE Refine
  - files: new `Engine/Nodes/Raw/RawDemosaicLmmse.cpp`, `Engine/Nodes/Raw/RawKernels.h`, `Engine/Nodes/Raw/RawDemosaic.cpp/.h`, new `Tests/Native/NativeRawDemosaicLmmse_Test.cpp`
  - approach:
    - Port `demosaicing/lmmse.c` `lmmse_demosaic`, `_init_lmmse_gamma` (a thread-safe once-init table, not darktable's globals) and `_median3f`.
    - The refine choice has 5 levels, default "median".
    - Map darktable's `DT_LMMSE_TILESIZE` tile with its overlap onto the RoI halo, aligned to the pattern, so tiled output equals the full frame.
    - No GPU path.
  - verify: gtest checks tiled = full frame bit for bit, plus the darktable comparisons for all 5 refine levels. Compiled at P7.T26.
  - size: L

- [ ] M80.P7.T21 — RawDemosaic: dual demosaic (RCD+VNG4, AMaZE+VNG4, Markesteijn 3-pass+VNG) with Dual Threshold and mask view
  - files: new `Engine/Nodes/Raw/RawDemosaicDual.cpp`, `Engine/Nodes/Raw/RawKernels.h`, `Engine/Nodes/Raw/RawDemosaic.cpp/.h`, new `Tests/Native/NativeRawDemosaicDual_Test.cpp`
  - approach:
    - Port `demosaicing/dual.c` `dual_demosaic` and `_slider2contrast`, with the `dt_masks_calc_detail_mask`/blur steps it calls.
    - Dual Threshold defaults to 0.2. Add a "Show Dual Mask" toggle, darktable's quad, default off (user call 3).
    - The halo is the larger of the two methods' halos plus the mask blur. AMaZE dual keeps AMaZE's 128-photosite tile alignment.
    - No GPU path.
  - verify: gtest checks tiled = full frame, plus the darktable comparisons for the three dual modes at 0.2 and 0.6. Compiled at P7.T26.
  - size: L

- [ ] M80.P7.T22 — RawDemosaic: X-Trans Frequency Domain Chroma
  - files: new `Engine/Nodes/Raw/RawDemosaicFdc.cpp`, `Engine/Nodes/Raw/RawKernels.h`, `Engine/Nodes/Raw/RawDemosaic.cpp`, new `Tests/Native/NativeRawDemosaicFdc_Test.cpp`
  - approach:
    - Port `demosaicing/xtrans.c` `xtrans_fdc_interpolate` (~518 onward) with its filter tables.
    - Map its tile and border onto a RoI halo aligned to the 6×6 period. Add a VNG fallback below the minimum size, as Markesteijn has.
    - Run an ASan pass on the standalone harness, as was done for Markesteijn.
    - No GPU path.
  - verify: gtest checks tiled = full frame, and a flat patch stays neutral at all 36 phases. The darktable comparison passes on the RAF. Compiled at P7.T26.
  - size: L

- [ ] M80.P7.T23 — RawDemosaic: capture sharpening
  - files: new `Engine/Nodes/Raw/RawDemosaicCapture.cpp`, `Engine/Nodes/Raw/RawKernels.h`, `Engine/Nodes/Raw/RawDemosaic.cpp/.h`, new `Tests/Native/NativeRawDemosaicCapture_Test.cpp`
  - approach:
    - Port `demosaicing/capture.c`: `_calc_auto_radius` (`_calcRadiusBayer`/`Xtrans`/`Mono`), `_get_variance_threshold`, `_prepare_blend`, `_modify_blend`, `_blur_mul`/`_blur_div` and `_calc_9x9_gauss_coeffs`, plus the iteration loop.
    - Knobs at darktable's defaults: Capture Sharpen off, Iterations 8, Radius 0 (auto), Contrast Sensitivity (the default comes from `_get_variance_threshold` per image), Corner Boost 0, Sharp Center 0.
    - The per-image auto radius and threshold are whole-frame scalars. Compute them once per `frameKey()` from the source mosaic and show the resolved values in the knobs' tooltips. Don't write them back.
    - The deconvolution is local: halo = the 9×9 kernel × iterations, bounded the way darktable's tiling overlap is.
    - Add "Show Sharpened Areas" and "Show Radius" views (user call 3).
    - No GPU path.
  - verify: gtest checks tiled = full frame. The auto radius equals darktable's logged autoradius for the CR2 and RAF within 1e-3. The darktable comparisons pass (fixed and auto). Compiled at P7.T26.
  - size: L

- [ ] ~~M80.P7.T24 — RawHotPixels: Mark Fixed Pixels~~ (cancelled 2026-10-23: deferred to the viewer-overlay milestone)
  - files: `Engine/Nodes/Raw/RawHotPixels.cpp/.h`, `Engine/Nodes/Raw/docs/RawHotPixels.md`, new `Tests/Native/NativeRawHotPixelsMark_Test.cpp`
  - approach:
    - Port the `markfixed` branches of `_process_bayer`/`_process_xtrans`/`_process_monochrome` (hotpixels.c:147, 204, 301): the darktable marker pattern, default off.
    - darktable never marks in thumbnails or exports (hotpixels.c:377); the scope in Natron is user call 3.
  - verify: gtest checks injected hot pixels get darktable's marker pattern on Bayer and X-Trans, and off is unchanged. There is no darktable render, because darktable disables marking on export. Compiled at P7.T26.
  - size: M

- [ ] M80.P7.T38 — RawDenoise: band positions (darktable's curve x)
  - files: `Engine/Nodes/Raw/RawDenoise.cpp/.h`, `Engine/Nodes/Raw/docs/RawDenoise.md`, `Tests/Native/NativeRawDenoise*_Test.cpp`, `tools/raw/make-reference.py` (one moved-x job, rendered in T2)
  - approach: darktable's `dt_iop_rawdenoise_params_t` stores x and y per band per channel (`x[DT_RAWDENOISE_NONE][DT_IOP_RAWDENOISE_BANDS]`); its GUI drags x (`x_move`). Natron exposes only y. Add per-band position knobs with darktable's defaults, feed them to the existing curve evaluation as darktable's `process` does, and keep the y knobs. Found by the P7.T3 audit.
  - verify: gtest: default x equals today's output bit for bit; a moved x changes the band weights as a hand evaluation of darktable's curve says; the darktable comparison of the moved-x job passes. Compiled at P7.T26.
  - size: M

- [ ] M80.P7.T25 — Large-image check for the new whole-frame and big-halo modes
  - files: `Tests/Native/RawLarge_Test.cpp`, `tools/bench/` (raw script only)
  - approach:
    - Run these on the synthetic 100 MP mosaic, in bands and as a whole source:
      - Flat Field;
      - Reconstruct Color, Segmentation and Laplacian highlights;
      - LMMSE, dual, FDC and capture sharpening with auto radius.
    - Check the `size_t` arithmetic, one whole-frame computation per `frameKey()` (count it, as the decode count is counted), and peak growth.
  - verify: the gtest passes under stated per-mode memory bounds, recorded in the README. The bench shows time falling with thread count for LMMSE and capture sharpening. Compiled at P7.T26.
  - size: M

- [ ] M80.P7.T26 — Build checkpoint B
  - files: (none new)
  - approach:
    - Run the syntax check, then remote debug `--tests` and the release gate, covering T19–T25 and T30–T36 plus anything reopened from A.
    - Re-run failures with `-V` and route them to their tasks.
  - verify: ctest is green on both legs, including every new P7 test, the RawColorCalibration darktable comparisons and GPU parity test, and `RawLargeTest`. T19–T25 and T30–T36 get checked off.
  - size: M

- [ ] M80.P7.T27 — Xvfb panels re-run and screenshots
  - files: `Tests/gui/m80_raw.py`, node panel layouts in `Engine/Nodes/Raw/*.cpp` (labels, pages and conditional visibility only)
  - approach:
    - Use the Xvfb recipe (`build/deeprepro/run-gui.sh`, `checkForUpdates` pre-seeded off) on the release build.
    - Every new knob has darktable's conditional visibility: Laplacian and segmentation knobs per mode, Dual Threshold only for dual modes, LMMSE Refine only for LMMSE, capture-sharpen knobs under a Capture Sharpen group, Finetune only for presets with tunings, Flat Field only with GainMaps. RawColorCalibration follows `_update_illuminants`: temperature for D/Planckian, F source for F, LED source for LED, hue/chroma for custom, all illuminant knobs hidden when adaptation is none, and adaptation disabled for as shot in camera. Its pages are CAT, R, G, B, Colorfulness, Brightness and Gray, plus Area Color Mapping and Calibrate With a Color Checker groups.
    - Script these steps: open the Bayer, X-Trans and linear-DNG fixtures; check Develop RAW holds six nodes with RawColorCalibration last and RawTemperature on As Shot to Reference; switch every new mode; pick a spot rectangle; choose a camera preset; on RawColorCalibration, switch every adaptation and illuminant, run Pick in both area modes, place the checker corner pin, press Calibrate and Validate, and apply a preset; toggle every mask view.
    - Screenshot each node panel per mode group (each RawColorCalibration page included), plus the viewer for each mask view and with RawColorCalibration's spot rectangle and checker corner pin, and share them with the user for approval.
  - verify: the script passes under Xvfb, and the user approves the screenshots.
  - size: M

- [ ] M80.P7.T28 — Final parity audit
  - files: `tools/raw/darktable-parity.json`, new `Tests/Native/RawParity_Test.cpp`, `Engine/Nodes/Raw/docs/*.md` (a "darktable controls" table per node, generated by the script)
  - approach:
    - Update the mapping from T3 for everything P7 added, and run `tools/raw/parity-audit.py` against `DARKTABLE_REF`.
    - `RawParity_Test` instantiates each node on each fixture's metadata and asserts every mapped knob exists with darktable's default, including the file-derived ones:
      - Flat Field on the GainMap DNG;
      - clip on the LRL DNG;
      - Embedded vs Standard matrix on DNG vs CR2;
      - X-Trans method Markesteijn 1-pass;
      - the capture-sharpen threshold from the image.
      - RawTemperature's As Shot to Reference default;
      - RawColorCalibration's adaptation, illuminant, temperature and hue/chroma per fixture (darktable's logged commit line from T29/T2), and pipe + none on a monochrome or already-adapted input.
    - Paste the table into the milestone's `## Decisions`.
  - verify:
    - The audit table has no MISSING rows; every row is a knob, an option or a justified N/A/EXTRA.
    - `RawParity_Test` passes. Compiled at P7.T37.
  - size: M

RawColorCalibration ports darktable's color calibration (`src/iop/channelmixerrgb.c`, params v3) as an image-in/image-out Point op in `Engine/Nodes/Raw/`, Tab menu `Raw/`, placed after RawInputColor in the project working space. Unlike the other raw nodes it is image kind and does not check `raw/stage`. It has every control darktable has, at darktable's defaults (`dt_iop_channelmixer_rgb_params_t`, channelmixerrgb.c:91-115, and `reload_defaults`, :3862):
- **CAT page:**
  - Adaptation: linear Bradford / CAT16 / non-linear Bradford / XYZ / none. Default CAT16; pipe + none on monochrome input or when a CAT is already applied upstream.
  - Illuminant: same as pipeline (D50) / A / D (daylight) / E / F / LED / Planckian / custom / as shot in camera. Default D. On a raw with a matrix it resolves per file to D, Planckian or custom.
  - F source F1–F12, default F3.
  - LED source B1–B5, BH1, RGB1, V1, V2, default B5.
  - Temperature 1667–25000 K (soft range 3000–7000), default 5003 or file-derived.
  - Hue° and Chroma%, stored as x, y; default 0.333/0.333 or file-derived.
  - Gamut compression 0–12, default 1.
  - Clip negative RGB, default on.
- **Mixing pages:** R, G and B rows of 3 coefficients each (−2…2), default identity. Colorfulness, Brightness and Gray rows, default 0. Six Normalize toggles, default off. Saturation algorithm v1/v2/v3, default v3.
- **Area color mapping:**
  - Area mode: correction / measure, default correction.
  - Take channel mixing into account, default on.
  - Target lightness 50, hue 0, chroma 0.
  - Area rectangle and Pick.
- **Color checker:**
  - Chart: 7 options, default Xrite ColorChecker 24 pre-2014.
  - Optimize for: 8 options, default none.
  - Patch scale 0.5.
  - Corner pin, Calibrate and Validate.
- **Presets:** darktable's 10 built-ins, with Apply.

Deferred as a display aid: the checker patch-grid and ΔE overlay (`gui_post_expose`, :2682). N/A:
- Detection from surfaces/edges: compiled out (`// #define AI_ACTIVATED`, :36).
- Cosmetic swatches, slider gradients and the CCT label (shown in a tooltip instead).
- Legacy-version import.

Order: T29 after T1 and before T2. T30 → T31 → T32 → T33 → T34 → T35 run serially, since they share the node's `.cpp`. T32 needs T5 and T14. T36 needs T14 and T32. T37 follows T27 and T28.

- [x] M80.P7.T29 — Add color calibration jobs to the reference harness
  - files: `tools/raw/make-reference.py`, `tools/raw/make-reference.sh`, `Tests/fixtures/raw/README.md`
  - approach:
    - Runs after T1 (same files) and before T2. Pack `dt_iop_channelmixer_rgb_params_t` at v3 (`DT_MODULE_INTROSPECTION(3, …)`, channelmixerrgb.c:70): six float[4] rows, six normalize gbooleans, illuminant, illum_fluo, illum_led, adaptation, x, y, temperature, gamut, clip, version. Add it as a `channelmixerrgb` history entry after colorin.
    - Jobs on the CR2 and RAF crops, temperature as shot unless stated:
      - each adaptation at D 4500 K (none with a non-identity mix);
      - each illuminant at CAT16: pipe, A, E, F2, F11, LED B1, LED RGB1, Planckian 3200 K, custom xy;
      - as shot in camera, with temperature as shot and with preset 4 (as shot to reference), which exercises `_get_white_balance_coeff`;
      - gamut 0/1/4 × clip on/off;
      - R/G/B mixing with normalize on and off;
      - colorfulness ±0.5 × v1/v2/v3;
      - brightness;
      - gray, normalized and not.
    - As shot in camera on the linear DNG (embedded matrix).
    - A scene-referred default job: `--conf plugins/darkroom/workflow=scene-referred (sigmoid)` with auto-presets applied, and history entries that disable sigmoid, exposure and sharpen. Record darktable's `-d params` "[commit color calibration] temp= xy=" line per fixture in the README. If the auto-presets don't apply under darktable-cli, pack the logged illuminant explicitly and say so.
  - verify: `python3 -I -m py_compile`; a dry run lists every job and writes every XMP; the README table lists each reference with its tolerance class. T2 renders them.
  - size: M

- [ ] M80.P7.T30 — Port darktable's illuminant and chromatic-adaptation maths
  - files: new `Engine/Nodes/Raw/RawColorCalibrationMath.h/.cpp`, new `Tests/Native/RawColorCalibrationMath_Test.cpp`
  - approach:
    - From `src/common/illuminants.h`: `illuminant_to_xy` with the fluorescent and LED tables, `CCT_to_xy_daylight`, `CCT_to_xy_blackbody`, `xy_to_CCT`, `CCT_reverse_lookup` (serial and deterministic, LUT built once and thread-safe), `xy_to_uv`, `find_temperature_from_raw_coeffs`, `WB_coeffs_to_illuminant_xy`, `matrice_pseudoinverse`, `illuminant_xy_to_XYZ`.
    - From `src/common/chromatic_adaptation.h`: `convert_any_XYZ_to_LMS`, `convert_any_LMS_to_XYZ`, `make_RGB_to_Bradford_LMS`, `make_Bradford_LMS_to_XYZ`, `make_RGB_to_CAT16_LMS`, `make_CAT16_LMS_to_XYZ`, `bradford_adapt_D50`, `CAT16_adapt_D50`, `XYZ_adapt_D50`, `convert_D50_to_LMS`, `chroma_adapt_pixel`.
    - The xyY/uvY/Lab/LCh conversions they use, and `_check_if_close_to_daylight` (channelmixerrgb.c:1216).
    - Reuse `RawColorMath::Mat3` where it fits, and keep darktable's header.
  - verify: gtest against a table produced by compiling the darktable headers standalone in /tmp: daylight and Planckian xy at several CCTs, the A/E/F/LED xy, CCT round trips, a CAT16/Bradford/XYZ adaptation of D50 that is the identity, and `find_temperature_from_raw_coeffs` on the CR2's metadata. Compiled at P7.T26.
  - size: L

- [ ] M80.P7.T31 — RawColorCalibration node: knobs, mixing and the per-pixel op
  - files: new `Engine/Nodes/Raw/RawColorCalibration.h/.cpp` + per-node files (`RawColorCalibration.reg.inc`, `Engine/Nodes/Raw/docs/RawColorCalibration.md`, icon), new `Tests/Native/NativeRawColorCalibration_Test.cpp`
  - approach:
    - Image in and out, Tab `Raw/`, with the CAT and mixing knobs from the intro at their static defaults.
    - `bake` ports `commit_params` (channelmixerrgb.c:3047): norms, MIX, the saturation/lightness/grey rows (v1 swaps R and B), gamut 1/g, the illuminant LMS, and p.
    - `row` ports `_loop_switch` (:771), `_gamut_mapping` (:648) and `_luma_chroma` (:706) exactly, including the NaN-aware max and the clip steps.
    - Working matrices: RGB→XYZ D50 = Bradford(D65→D50) × `ProjectColorManagement::getWorkingToXYZ` (D65, ProjectColorManagement.h:162). darktable's pipe is ICC D50, so "same as pipeline" stays the identity. Fall back the way `RawInputColorOp::xyzToWorking` does.
    - The Hue/Chroma knobs map to x,y through xyY↔LCh.
    - Visibility follows `_update_illuminants` (:3175). `knobChanged` ports `gui_changed` (:3981): it remembers the daylight and Planckian temperatures, recomputes x,y on an illuminant, F, LED or temperature change, and updates the temperature from xy.
    - `isIdentity` when nothing would change a pixel.
  - verify: `expectNativeIdentity`; pipe illuminant + identity mix + gamut 0 + clip off is the identity within 1e-6; the darktable comparisons for every T29 job except the camera and scene-default ones, with the test project's working space set to linear Rec709 as the harness uses. Compiled at P7.T26.
  - size: L

- [ ] M80.P7.T32 — RawColorCalibration: file-derived defaults, "as shot in camera", double-CAT and white-balance warnings
  - files: `Engine/Nodes/Raw/RawColorCalibration.cpp/.h`, `Engine/Nodes/IO/RawMetadata.h/.cpp` (`raw/cat_applied`, plus getters for `raw/wb_d65_coeffs` and `raw/wb_late_correction` if T14 left any out), new `Tests/Native/NativeRawColorCalibrationDefaults_Test.cpp`
  - approach:
    - Port `reload_defaults` (:3862) with RawPrepare's shown-until-overridden pattern:
      - A raw input with a matrix gets CAT16. `find_temperature_from_raw_coeffs` and `_check_if_close_to_daylight` then pick D, Planckian or custom, plus the temperature and x,y.
      - Monochrome input, or `raw/cat_applied` upstream (darktable's first instance owns the CAT, `_declare_cat_on_pipe` :1173), gets pipe + none.
      - Other input keeps D 5003 K and CAT16.
    - The node publishes `raw/cat_applied` while it adapts.
    - "As shot in camera" is offered only when the metadata allows it. It re-resolves on every render, as `process` does (:2190):
      - Port `_get_white_balance_coeff` (:614) and `_dev_is_D65_chroma` on `raw/wb_coeffs` (as shot), `raw/wb_applied`, `raw/wb_d65_coeffs` and the presence of `raw/wb_late_correction`.
      - The matrix is `raw/dng_d65_matrix` (T5) if present, else `raw/cam_to_xyz` (stored as the inverse of rawspeed's XYZ→CAM, RawDecodeSupport.cpp:560).
      - Adaptation is disabled and forced to CAT16 while the illuminant is the camera's.
    - Port `_set_trouble_messages` (:1981) as node warnings: double CAT, white balance applied twice, and a RawTemperature not at reference or as shot to reference.
  - verify: the defaults on the CR2, RAF and DNG equal darktable's logged commit line from T29 (temperature within 1 K, xy within 1e-4); the darktable comparisons for the camera-illuminant jobs (as shot and preset 4) pass; a second instance downstream defaults to pipe + none and warns. Compiled at P7.T26.
  - size: L

- [ ] M80.P7.T33 — RawColorCalibration: area color mapping (spot) and built-in presets
  - files: `Engine/Nodes/Raw/RawColorCalibration.cpp/.h`, new `Engine/Nodes/Raw/RawColorCalibrationSpot.cpp`, `Engine/Nodes/Raw/docs/RawColorCalibration.md`, new `Tests/Native/NativeRawColorCalibrationSpot_Test.cpp`
  - approach:
    - Spot area bottom-left/size knobs with `node->addRectangleInteract` (ExtentKnobs.cpp:276).
    - Area Mode (correction/measure, default correction), Take Channel Mixing Into Account (on), Target Lightness 50 / Hue 0 / Chroma 0.
    - A Pick button reads the input mean over the area at full scale (Grade's `normalize()` pattern, as T13), then ports `_auto_set_illuminant` (:4161). Measure writes the target L/h/c. Correction writes x,y, custom illuminant and temperature.
    - Snapshot semantics. The targets are per-node knobs, where darktable keeps them in global conf.
    - A Preset choice plus an Apply button port `init_presets`' 10 entries (:387): six monochrome film presets, basic channel mixer, and swaps of G/B, G/R and R/B.
  - verify: on a synthetic patch, correction maps the area to the target within ΔE 0.1 with and without mixing; measure followed by correction on the same area is a no-op; each preset writes darktable's values. No darktable render, because the picker is GUI-only. Compiled at P7.T26.
  - size: M

- [ ] M80.P7.T34 — RawColorCalibration: color-checker calibration
  - files: new `Engine/Nodes/Raw/RawColorCalibrationChecker.h/.cpp`, new `Engine/Nodes/Raw/RawColorCheckerData.h` (from `src/common/colorchecker.h`), `Engine/Nodes/Raw/RawColorCalibration.cpp/.h`, new `Tests/Native/NativeRawColorCalibrationChecker_Test.cpp`
  - approach:
    - A corner pin through `Node::addCornerPinInteract` (Node.h:1536): hidden "from" knobs at the ideal box, visible "to" knobs on the chart corners.
    - Chart (7 options, default Xrite pre-2014), Optimize For (8 options, default none), Patch Scale 0.5.
    - Calibrate reads the input at full scale and ports `_extract_patches` (:1426), using `get_homography` from `src/chart/common.c:23`. It then ports `_extract_color_checker` (:1642), with `pseudo_solve_gaussian` from `src/iop/gaussian_elimination.h` and the per-strategy patch weights, and commits as `_commit_profile_callback` (:2933): custom x,y, temperature, and the R/G/B rows.
    - Validate ports `_validate_color_checker` (:1939) and `_compute_patches_delta_E` (:1288). Average and maximum ΔE 2000 go in the node info.
    - The patch-grid overlay is deferred.
  - verify: a synthetic chart (each chart's reference Lab through a known illuminant and 3×3) is recovered within tolerance; the ΔE report equals a hand computation; all 7 charts give the right patch counts and layouts. Compiled at P7.T26.
  - size: L

- [ ] M80.P7.T35 — GPU kernel for RawColorCalibration
  - files: new `Engine/Gpu/Kernels/rawcolorcalibration.slang`, new `Engine/Gpu/Kernels/rawcolorcalibration.cmake`, `Engine/Nodes/Raw/RawColorCalibration.cpp`, `Engine/Nodes/Raw/RawGpuParams.h`, new `Tests/Native/NativeRawColorCalibrationGpu_Test.cpp`, `Tests/Native/NativeGpuCapability_Test.cpp` (allowlist)
  - approach:
    - Mirror `rawinputcolor.slang` and `RawInputColorOp::gpu`.
    - The CPU bakes the parameter block: RGB→LMS, MIX→XYZ and XYZ→RGB matrices, illuminant LMS, p, gamut, the saturation/lightness/grey rows, and the adaptation, version, clip and apply-grey flags.
    - The kernel switches on adaptation and keeps the CPU's NaN-aware clipping and `powf` semantics.
    - The camera illuminant is resolved in `bake`, so the kernel never sees it. Spot, checker and presets only write knobs.
  - verify: CPU/GPU parity per adaptation × version × clip × gamut on RADV/llvmpipe, at RawInputColor's GPU tolerance class; `NativeGpuCapabilityTest` allowlists the node. Compiled at P7.T26.
  - size: M

- [ ] M80.P7.T36 — Develop RAW: insert RawColorCalibration; RawTemperature defaults to As Shot to Reference
  - files: `Engine/Nodes/Raw/RawDevelop.h/.cpp`, `Engine/Nodes/Raw/RawTemperature.cpp`, `Engine/Nodes/Raw/docs/RawTemperature.md`, `Tests/Native/RawDevelop_Test.cpp`, new `Tests/Native/RawReferenceSceneDefault_Test.cpp`
  - approach:
    - `Chain` gains `colorCalibration` after `inputColor`: the slot arrays at RawDevelop.cpp:119 and :154, `addNodesToCreationOf` (:163), and the backdrop height.
    - RawTemperature's own mode knob default becomes As Shot to Reference for every instance, not only inside Develop. This is darktable's default under its default `scene-referred (sigmoid)` workflow (temperature.c:1518, `darktableconfig.xml.in` workflow). The user approved it on 2026-10-23, superseding user call 1.
    - The docs say the mode only white-balances when a CAT follows, and that RawColorCalibration warns otherwise.
  - verify: RawDevelop_Test checks six nodes in order, linked, inside the backdrop; a fresh RawTemperature defaults to As Shot to Reference; the whole default chain on the CR2 and RAF matches T29's scene-referred default job within the README class; the linear DNG develops. Compiled at P7.T26.
  - size: M

- [ ] M80.P7.T37 — Build checkpoint C
  - files: (none new; fixes go to the owning task's files)
  - approach:
    - Run `tools/ci/local/syntax-check.py` on the changed TUs, then one remote `tools/ci/remote/build.sh debug --tests` and one release gate.
    - Covers T27's panel layout edits, T28's `RawParity_Test`, and anything reopened from B.
    - Re-run failures with `-V` and route each to its task.
  - verify: ctest is green on both legs, including `RawParity_Test` and every `*RawColorCalibration*` test. T27 (after user approval) and T28 get checked off.
  - size: M

**Verification gate:**
- `ctest` is green, including every new `Tests/Native/*Raw*`, `NativeReadRaw`, `NativeSourceKind`, `RawKindFramework`, `RawParity` and core-chain GPU parity test, `NativeRawColorCalibrationGpu` included. The GPU tests run on RADV/llvmpipe as the existing GPU tests do.
- Every darktable control (each `dt_iop_*_params_t` field, enum value and output-changing GUI control of rawprepare, temperature, highlights, demosaic, colorin, hotpixels, cacorrect, rawdenoise and channelmixerrgb at `release-5.6.2`) has a knob with darktable's default, including file-derived defaults. The parity audit table (`tools/raw/parity-audit.py`, M80.P7.T28) has no MISSING rows.
- The darktable reference comparisons (M80.P5.T1, M80.P7.T2) pass within their stated tolerances for every mode darktable-cli can render, on the Bayer, X-Trans, linear-DNG and synthetic fixtures. The modes it can't render (spot white balance, preset selection, color calibration's area mapping, checker calibration and presets) have hand-computed tests.
- Develop RAW inserts RawColorCalibration after RawInputColor, RawTemperature defaults to As Shot to Reference, and the default chain matches darktable's scene-referred default render (M80.P7.T36).
- Every mode added in Phase 80.7 reports no GPU path and renders on the CPU (GPU in M81 - Raw GPU Kernels), except RawColorCalibration, which has a GPU kernel with a CPU parity test (M80.P7.T35).
- The large-image test (M80.P5.T2, extended by M80.P7.T25) passes, including the new whole-frame modes, within its stated memory bounds.
- A linear DNG reads as raw and develops.
- An unsupported-camera stub shows the named error.
- The Xvfb script (M80.P5.T3, re-run by M80.P7.T27) passes, and the user has approved the screenshots.

## Decisions

- 2026-10-22 — Core-chain GPU kernels move into this milestone (Phase 80.6): the backend is settled (Vulkan + Slang) and the round-two rule has ports ship their own kernels; estimated at about 6 tasks of the 12–15 for all raw GPU work. The rest stays in M81 - Raw GPU Kernels (user).
- 2026-10-22 — M80.P6.T1 (backend requirements note) cancelled: M83 - GPU Compute Backend Spike already chose the backend.
- 2026-10-22 — Lens correction leaves this milestone for a new M160 - Lensfun Lens Correction, which depends on this milestone and on M113 - Native Warp Nodes' coordinate sampler. It stays a separate node from M132 - Native Lens Distortion (user).
- 2026-10-22 — Develop RAW is loose nodes in a backdrop, permanently. Groups are an image barrier; M159 - Kind-Agnostic Groups fixes groups in general, but Develop does not move to a Group (user).
- 2026-10-22 — Develop RAW auto-inserts only on interactive creation, behind a preference that defaults on (user).
- 2026-10-22 — Linear and no-CFA raw stays raw kind with 3 or 1 channels; Demosaic passes it through. A new RawToImage node does a plain raw→image conversion (user).
- 2026-10-22 — The Viewer accepts raw edges directly, through the same conversion as RawToImage (user).
- 2026-10-22 — A wrong `raw/stage` gives a persistent error and no render; Prepare twice passes through with a warning (user).
- 2026-10-22 — A camera unknown to the pinned rawspeed gives a persistent Read error naming it; there is no fallback decoder (user).
- 2026-10-22 — One darktable release tag pins every port and the reference harness (user).
- 2026-10-22 — No adapter row for raw: the README says conversions are explicit, and Demosaic and RawToImage declare raw→image themselves (code: `Node.cpp:1892`, README "Conversions between kinds are explicit").
- 2026-10-22 — The Read's knob-driven kind uses the existing polymorphic `resolveOutputDataKind` hook with explicit cache invalidation (`NativeEffectBase.h:244-253`), so P2.T2 no longer carries a RawRead-node fallback.
- 2026-10-22 — Nodes are built on the M137 framework with the M120 per-node files; `RowIO::x0/y` gives Point ops the CFA position (code: `PixelKernel.h:69`).
- 2026-10-22 — **Claimed despite strict file overlap** (user): run alongside M80, M16 and M158 in parallel lanes, with M64 - Tiled Rendering (parked) not treated as a blocker; overlaps with active rows (M86, M107, M154) and sibling lanes are one or two shared files, handled by rebasing after each merge.
- 2026-10-22 — Freshness check (§5a) at claim: stale file lists and line numbers corrected in place; no premise change.
- 2026-10-22 — Pins: `DARKTABLE_REF` = `release-5.6.2` (`20891f6c6fa7`, latest published release); `RAWSPEED_REF` = `c835b05aecfa`, the submodule darktable 5.6.2 pins, so the decoder matches the ported release. pugixml 1.16 comes from the image.
- 2026-10-22 — rawspeed installs to `build/assets/rawspeed/`, not `plugin-src/deps-install/`: CI caches only `build/assets/{Plugins,TestPlugins,CImg}`, so it is added to all six workflow cache lists. Static, `BINARY_PACKAGE_BUILD=ON`, OpenMP off; the script writes its own `rawspeedConfig.cmake` because rawspeed has no install rule. `cameras.xml` ships at `Resources/rawspeed/`.
- 2026-10-22 — `raw/black_levels` holds one value per cell of the 2×2 block at the origin (rawspeed's layout, separate greens), or one per channel for linear data; the origin is a separate `raw/cfa_origin` int pair. Coordinates are Natron's (y up); the Read converts from file order.
- 2026-10-22 — P1.T4 stopped at its scouting gate: ops' `bake()` sees only a `KnobReader`, so a raw op cannot read the CFA pattern or levels from source metadata. Framework change sent to a consultant before implementing.
- 2026-10-22 — raw.pixls.us is unreachable from this host and from WebFetch (connection refused); the user will supply the three CC0 fixtures into `build/raw-fixtures/` with their source URLs. P2.T1, T3–T5 wait for them; P2.T2 (needs none) proceeds. rawspeed can't read a header without a full decode, so a header-cache miss decodes the image and feeds it straight into the bounded decode cache.
- 2026-10-22 — Fixtures supplied by the user (raw.pixls.us, CC0): Canon EOS 5D Mark III CR2 (Bayer, 26 MB), Fujifilm X-Pro2 14-bit uncompressed RAF (X-Trans, 51 MB), Adobe DNG Converter linear RGB DNG of the 5D Mark III (64 MB), at `Tests/fixtures/raw/` in the m80 lane. ~140 MB total, so they are tracked with **Git LFS** (user); CI checkouts that run ctest need `lfs: true`.
- 2026-10-22 — Phase 80.3 runs before Phase 80.2's decoder tasks, against synthetic mosaics, while fixtures were pending; fixture-based checks are added when P2.T1 lands.
- 2026-10-22 — Phase 80.3 code is in `22794001a`, unchecked until a green build. AMaZE aligns its RoI to its own 128-photosite tile grid instead of an 18 px halo, because no fixed halo made tiled output equal the full frame. Markesteijn's mirrored edge fill reads out of bounds below 14 photosites a side (1-pass) or 19 (3-pass), so the node falls back to VNG there. Develop RAW hooks `NativeRead::onEffectCreated`, the one point every creation path shares, and `RawDevelop::isInteractiveArgs` filters out non-interactive creations. Known gaps for the review round: the chain is inserted without an undo command; the five `.costseed.inc` files have no rows, and their headers wrongly say `render-profile-report.py` wrote them.
- 2026-10-22 — Phase 80.2 code is in `31c899f9b`. The fixes from the Phase 80.3 debug build are in `47057bd59`: a VNG border loop that never ended on narrow buffers, a test helper's channel count, a RawDevelop test bug, and the cost-seed fragments deleted because row-less fragments fail the self-test. The fixtures went into Git LFS in `4bf29ce0f`, once the fork was detached from NatronGitHub/Natron; a fork in a public network can't store LFS objects of its own. The CI containers have no git-lfs, so `tools/ci/local/install-git-lfs.sh` installs a pinned v3.8.1 in the dev image and in the release and nightly jobs. CI restores `.git/lfs` from `actions/cache` keyed on the LFS object IDs (user's choice, to save LFS bandwidth). This host can't reach GitHub's LFS storage on S3, so the objects were uploaded from elsewhere. CR3 isn't claimed, because the pinned rawspeed has no CR3 decoder. RawToImage outputs RGBA with grey RGB and alpha 1, since a one-channel Natron image is alpha-only. The exact raw.pixls.us file URLs are still missing from the fixture README; it links each camera's directory and gives SHA-256s.
- 2026-10-22 — Phase 80.4 code is in `89d3a2b32`, unchecked until a green build. RawCACorrect caches the whole corrected mosaic per frame key, not just the shift estimate: darktable's second iteration measures the frame the first one corrected, so caching shifts alone would still re-run every iteration over the whole frame for each tile. The whole-frame key is shared through `RawNodeSupport::frameKey()` with RawDemosaic and RawHighlights. At release-5.6.2 darktable's rawdenoise reads no noise profiles and no ISO, and has one method, so none of those ship (P4.T3's profile data, method knob and ISO override are dropped). RawDenoise's X-Trans tiling halo is 66 photosites, because darktable never fills green in the last column. The CA fit needs about 32 usable 112 px blocks, a RawTherapee limitation kept as-is.
- 2026-10-22 — Resumed after a host reboot. The debug build of `89d3a2b32` failed only `ViewerRawTest.ViewerAcceptsARawReadAndOtherImageInputsDoNot`; the fix `528097479` is building remotely (run 39883542171). Phase 80.6 was re-dispatched as one opus unit over the stopped implementer's uncommitted kernels and tests. The rebase onto main (M154 touched only `remote-build.yml` of this lane's files) waits until that unit is committed, so one build covers both. P5.T1 (darktable in a container) and the Xvfb checks need Docker or a native recipe; they are not yet attempted this session.
- 2026-10-22 — Remote debug build of `528097479`: ctest 348/348 on both legs, so Phase 80.1–80.4 code (`31c899f9b` through `528097479`) builds and passes, including `ViewerRawTest`. P2.T4 and P3.T8 stay unchecked until their Xvfb checks run; P3.T3 waits on the P5.T1 reference fixture.
- 2026-10-22 — Phase 80.6 code is in `100acfe47` (rebased onto main `448013710`), unchecked until a green build; remote debug build dispatched. The re-dispatched implementer kept the stopped one's work, added the missing `demosaic_bayer.cmake`, renamed Slang locals named `out`, and made `makeGpuQueryForTask()` refuse full-scale-only nodes at proxy levels (they were already refused at region build). `GpuBuffer`/`GpuWrap` needed no change for one channel. The CFA layout travels in each kernel's parameter block (the point-op convention), not a separate uniform buffer. The test-only `rawlayout_probe` entry lives in `rawprepare.slang`, because slangc runs without `-I`. The PPG/RCD reference is `RawDemosaicBasics.cpp` (`DemosaicBayer.cpp` doesn't exist).
- 2026-10-22 — GPU coverage limits in Phase 80.6: Highlights "Inpaint Opposed" (the default mode, a double-precision whole-frame chroma sum), green equilibration and colour smoothing have no GPU path. So **a default Develop chain renders on the CPU**; the resident-chain test uses Clip and LCh with RCD. The cost model can still split a raw run, and a Dot between raw nodes splits it. Raw nodes have no cost-seed rows (Grade's and Blur's rows apply).
- 2026-10-23 — Resumed. Lane rebased onto main `bf781cbcd` (M158), no conflicts. The Phase 80.6 build failed only `NativeGpuCapabilityTest.OnlyAllowlistedNativeNodesRenderOnGpu` (RawInputColor now has a GPU path); allowlisted in `2586de5f0`. P5.T1 harness and references are in `54f2a4e19`, unchecked until a green build: darktable 5.6.2 built in a Debian trixie container (Ubuntu's archive unreachable here; LibRaw can't be disabled at 5.6.2, so it is built but unused); 192×192 crops of the CR2 and RAF; tolerances relative to the crop mean, 0.1%/2% (PPG, RCD) and 0.3%/5% (AMaZE, Markesteijn), with darktable's own method-to-method spread for scale in the README. darktable has no "unclipped" highlights mode (that reference is the module off) and can't export a mosaic (the highlights references use the photosite-colour debug demosaic). P3.T3's darktable comparison is `NativeRawHighlightsTest.MatchesTheDarktableReference`. LFS objects uploaded from this host this time.
- 2026-10-23 — Debug build of `54f2a4e19` (run 40037813655): 356/357 on both legs. Phase 80.6 GPU parity tests and `NativeRawHighlightsTest.MatchesTheDarktableReference` pass, so P3.T3 and P6.T2–T6 are checked. `RawReferenceTest`: PPG, RCD, AMaZE and Markesteijn 1-pass match darktable to 0.013–0.015% mean and 0.11–0.12% max; Markesteijn 3-pass fails on one pixel (crop 95,3: 16.7% against a 5% max), sent to a consultant. The previous build's `SchedulerEquivalence` segfault was a pre-existing `AbortableRenderInfo` timer race (logged to the inbox).
- 2026-10-23 — User decision: the Viewer normalises a raw edge for display (black subtracted, divided by the white level); RawToImage keeps raw sensor values. Added as P5.T4. The Xvfb run (`a94ad2336`) passed 192 checks; RawDenoise tooltips and InputColor's r/g/b matrix columns were fixed in source. The user asked how far the knobs match darktable's controls before signing off P5.T3; a parity audit is running. `RawReferenceTest` passes with the Markesteijn outlier budget (`b063b28d4`); `RawLargeTest` found no in-flight decode dedupe (5 decodes for 5 concurrent requests) and 2.06 GB peak growth for two bands of a 100 MP mosaic, sent to a consultant.
- 2026-10-23 — **User decision: a port means every control.** Each ported darktable module (rawprepare, temperature, highlights, demosaic, colorin, hotpixels, cacorrect, rawdenoise) gets a knob for every user-facing control of its `dt_iop_*_params_t` and GUI at `release-5.6.2`, with darktable's defaults, including defaults darktable derives from the file. Earlier tasks enumerated a subset of modes and controls (e.g. highlights clip/opposed/LCh; demosaic PPG/RCD/AMaZE/Markesteijn/VNG) and nobody checked them against darktable's params; a parity audit on 2026-10-23 found the gaps. Phase 80.7 closes them. GPU kernels for the newly ported modes go to M81 - Raw GPU Kernels, like the other non-core-chain kernels. The milestone's existing exclusions stay: ICC/DCP profile files, SSE/OpenCL code paths, lens correction (M160), multi-frame raw.
- 2026-10-23 — Phase 80.7 planned from the parity audit (consultant). User calls: spot white balance is a rectangle on the viewer plus a Pick button that stores coefficients once; colorin's Working Profile defaults to the project working space plus darktable's options; 4-colour (CYGM/RGBE) controls are N/A because the Read rejects 4-colour sensors; file-derived defaults use RawPrepare's show-the-file's-value-until-overridden pattern, and choice defaults resolve per file (Flat Field Embedded when GainMaps exist; Input Profile Embedded for a DNG, Standard otherwise); `wb_presets.json` ships in the repo under `Engine/Nodes/Raw/data/`, installed to `Resources/darktable/`.
- 2026-10-23 — **User decision: port darktable's color calibration as a node** rather than default to As Shot. With it, the Develop chain can use darktable's scene-referred default (As Shot to Reference plus a CAT). Tasks to be added to Phase 80.7. Open: how darkroom-only display aids (mask views, dual mask, capture-sharpen masks, hot-pixel markers) show — the user asked whether nodes can draw arbitrary viewer overlays; being scouted.
- 2026-10-23 — User decision: defer darktable's darkroom-only display aids (mask views, dual mask, capture-sharpen views, hot-pixel markers) until Natron has rich viewer overlays (overlaid images, handles, arbitrary drawing). Today native nodes can only use stock host overlays (no custom `drawOverlay`), no overlay can draw per-pixel data, and nodes can't query the existing viewer-render flag (`isRenderUserInteraction`). A milestone for that was requested through the inbox.
- 2026-10-24 — Fast build with tests of `7be5ffca2` (run 40048743972) green on both legs: `RawReferenceTest` (with the Markesteijn outlier budget), `RawLargeTest` (decode once; separate band and whole-source memory bounds) and the Viewer normalisation pass. P5.T1, P5.T2, P5.T4, P2.T4 and P3.T8 checked; P5.T3 waits for the Phase 80.7 panel re-run (T27) and the user's screenshot approval. User calls on color calibration: RawTemperature's own default becomes As Shot to Reference; the node ships its GPU kernel here; it lives in the Raw group as `RawColorCalibration`.
- 2026-10-23 — P7.T12: As Shot's missing-white-balance fallback follows darktable's `_find_coeffs` (colour-matrix D65, then the camera's first preset row, then 2/1/1.5), not the task text's "Daylight, then D65" order; Camera Reference uses `reload_defaults`' order (matrix, then the tuning-0 daylight/direct-sunlight row). darktable's own code wins over a brief's paraphrase. P7.T10–T12 committed (f2ee98118, fe8caea68, 4fb3a8e37), checked off after P7.T18.
- 2026-10-23 — P7.T3 audit (code 57e04c547): 84 MISSING rows — 60 channelmixerrgb (T30–T36), 23 demosaic (T19–T23), 1 rawdenoise band x (new T38). Natron's flat-field default matches darktable's `reload_defaults` (embedded when the file has gain maps); colorin's default is file-derived per T15.
- 2026-10-24 — **Checkpoint A (P7.T18):** debug `--tests` on ca3bd3ca2 371/371 green (after four test-only fixes: highlights tests swapping sources without disconnecting and missing the clip override; input-colour tests feeding raw data straight into RawInputColor); release `--tests` on bdc363e05 372/373 — the one failure is T20's new `NativeRawDemosaicLmmseTest.TilesMatchTheFullFrameWithLocalGreensAndSmoothing` (render returns no plane, then the test segfaults), routed to T20. The regenerated references (T2, fb98e7c19) pass the existing `RawReferenceTest` and highlights comparisons. T2, T5–T17 and T19 checked off; T4 remains (not yet implemented).

## Handoff (2026-10-22, PM paused by user)

- Lane: `.worktrees/m80`, branch `milestone/m80-native-raw-support`, HEAD `efae6a05a`, pushed. Done and checked: P1.T1–T3, P2.T2. P1.T4 code is in (`cee20bb63`); its only CI failure (`RawKindFrameworkTest.DeclaredKindsResolve`, a test bug) is fixed in `efae6a05a` but **not yet built** — check P1.T4 off after the next green build.
- **Uncommitted in the worktree:** a stopped Phase 80.3 implementer's partial work (`Engine/Nodes/Raw/`, RawPrepare/Temperature/Highlights/Demosaic/InputColor tests and icons, `RawDevelop_Test.cpp`, `RawTestSource.h`, `Tests/gui/m80_raw.py`, edits to `AppManager.cpp`, `NativeRead.cpp`, `RawMetadata.{h,cpp}`, `Settings.{h,cpp}`). Unknown completeness (it was mid-way through demosaic tests): review it with a fresh implementer that finishes Phase 80.3 rather than trusting it as done.
- **Also uncommitted:** the three user-supplied fixtures in `Tests/fixtures/raw/` (CR2, RAF, linear DNG; ~140 MB). They must be committed via **Git LFS** (`.gitattributes`), with `lfs: true` added to every CI checkout that runs ctest, plus `Tests/fixtures/raw/README.md` (raw.pixls.us, CC0). That's the first step of the remaining Phase 80.2 unit (P2.T1, T3, T4, T5).
- `syntax-check.py` can't configure this worktree (no local rawspeed); implementers ran `-fsyntax-only` with the main checkout's `build/syntax/compile_commands.json` re-pointed here, rawspeed headers at `build/rawspeed-src`.
- Late addition: a helper agent of the stopped Phase 80.3 implementer finished X-Trans work, also uncommitted: `Engine/Nodes/Raw/RawDemosaicMarkesteijn.cpp` (`RawKernels::markesteijn()`, tile 122, no FDC) and `RawDemosaicVng.cpp` (`RawKernels::vng()` for Bayer VNG4 and X-Trans, `vngLinearInterpolate()`). Checked standalone in `/tmp/xtrans-check/`: flat patches neutral at all 36 phases, tiled = full frame, threaded = serial. Halos: Markesteijn 1-pass 12, 3-pass 18, VNG 6 for both variants. Move the `vngLinearInterpolate` declaration into `RawKernels.h` (it is declared locally in the Markesteijn file). The Markesteijn paths have had no ASan run.
