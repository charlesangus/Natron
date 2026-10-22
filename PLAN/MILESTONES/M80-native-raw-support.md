# M80 - Native RAW Support

Brings RAW camera files back as a native pipeline ported from darktable. rawspeed decodes the file, and the native Read emits the undemosaiced sensor data on a new `eDataKindRaw` edge. Native nodes ported from darktable's CPU modules then turn it into a scene-linear image: rawprepare, temperature, highlights, demosaic and input colour, plus hot pixels, chromatic aberrations and raw denoise. A plain RawToImage converter and a viewer that shows raw directly let the user look at any stage. The core chain gets its GPU kernels here (Phase 80.6). The rest of the raw GPU work is in M81 - Raw GPU Kernels, and lens correction is its own milestone, M160 - Lensfun Lens Correction.

This supersedes the "RAW camera formats are dropped" clause of `DECISIONS/2026-10-09-native-read-format-scope.md`. The "different technique, not OIIO's libraw plugin" that clause anticipated is rawspeed plus these nodes. It also revises `DECISIONS/2026-10-09-raw-support-via-rawspeed-and-darktable-ports.md`; see `DECISIONS/2026-10-22-raw-support-grill-revisions.md`.

Licensing: darktable's modules are GPL-3.0-or-later, rawspeed is LGPL-2.1, and Natron is GPL-2-or-later. Ported files keep their darktable copyright headers, and the combined work is distributed under GPL-3 terms. Record this in `tools/license/components/README.md`.

Every node is built on the M137 - Native Base Class Flexibility framework (`NodeSpec`, `ImagePipelineSpec` op kinds, `StagePlan`). Each node adds the per-node files from M120 - Native Node Port Kit, following the Nodes README checklist "Adding a native node". Mosaic parameters travel as M74 - Native Metadata Core image metadata. The reader structure is M75 - Native Read's.

## Scope

**In:**
- rawspeed decode, pinned. Read emits raw kind for every RAW file: a 1-channel CFA mosaic, or 3 or 1 channels for linear DNGs and other files with no CFA.
- The `eDataKindRaw` data kind, with raw-kind nodes on the native framework.
- CPU ports of rawprepare, temperature, highlights, demosaic (PPG, RCD, AMaZE, Markesteijn, VNG for X-Trans), input colour, hot pixels, CA correct and raw denoise.
- GPU kernels and parity tests for the core chain: the mosaic in GPU regions, Prepare, Temperature, Highlights, Demosaic PPG and RCD, and InputColor.
- A RawToImage node: a plain raw→image conversion with no processing.
- The Viewer accepts raw edges directly.
- Develop RAW is inserted automatically, as loose nodes inside a "Develop RAW" backdrop.
- A darktable reference harness, a large-image check, and Xvfb panels with screenshots.

**Out:**
- GPU for Markesteijn, AMaZE, VNG, hot pixels, CA, denoise: M81 - Raw GPU Kernels.
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

- [ ] M80.P1.T4 — Host raw-kind nodes on the native framework
  - files: `Engine/Nodes/NodeSpec.h`, `Engine/Nodes/Image/ImagePipelineSpec.h`, `Engine/Nodes/Image/StagedImageEffect.h/.cpp`, `Engine/Nodes/README.md`, new `Tests/Native/RawKindFramework_Test.cpp`
  - approach: `StagedImageEffect` ops are image-kind today. Let an `ImagePipelineSpec`/`NodeSpec` declare raw in with raw out, or raw in with image out. Ops already see pixel position through `RowIO::x0`/`y` (`Engine/Nodes/Image/PixelKernel.h:69`), so a Point op can call `RawLayout::colourAt`. Stages that make no sense on a mosaic are rejected at compile time for raw-in specs: Premult, and the layer and channel selection. The framework checks `raw/stage` through `RawMetadata::requireStage` before rendering and raises the persistent error. Scout first: if raw-in specs need more than a kind field plus those restrictions, report it before implementing. Document raw nodes in the README "Flat image nodes" section.
  - verify: gtest with a test-only raw Point op: declared kinds resolve; a raw→raw op sees the right `colourAt` per row; a raw→image op connects to an image consumer; a wrong `raw/stage` sets the persistent message and renders nothing.
  - size: L

## Phase 80.2: Decode and read

- [ ] M80.P2.T1 — rawspeed decode support
  - files: new `Engine/Nodes/IO/RawDecodeSupport.h/.cpp`, new `Tests/Native/RawDecodeSupport_Test.cpp`, new fixtures under `Tests/fixtures/raw/`
  - approach: model it on `OiioReadSupport`. A header cache keyed on (path, mtime, size) holds the parsed `RawImage` metadata behind a mutex. `decode(path, RectI window, float* dst, stride)` copies the window as float. rawspeed decodes whole files, so keep one decoded `RawImage` per cache key in a size-bounded LRU. The cap is a fraction of the project cache, not a constant, so it suits many-core high-RAM machines and modest ones alike. Fill `RawMetadata` from the decoder: CFA (`none` and the real channel count for linear DNGs), black and white levels, as-shot white balance, the camera matrix from `cameras.xml`, and the crop. A camera rawspeed rejects or marks unsupported fails with an error naming the make, model and rawspeed version. All offsets are `size_t`. Fixtures: small CC0 raw files from raw.pixls.us — a Bayer file, an X-Trans RAF and a linear DNG. Record their sources in `Tests/fixtures/raw/README.md`.
  - verify: gtest checks dimensions, CFA pattern, channel count, levels and crop against rawspeed's own output for each fixture. A window decode equals the same window of the full decode, bit for bit. A truncated file fails cleanly, and an unsupported-camera stub gives the named error.
  - size: M

- [x] M80.P2.T2 — Let the native Read resolve its output kind from the file
  - files: `Engine/Nodes/IO/NativeRead.cpp/.h`, `Engine/Nodes/README.md`, new `Tests/Native/NativeSourceKind_Test.cpp`
  - approach: the mechanism already exists. A node declared `eDataKindPolymorphic` is resolved through `NativeEffectBase::resolveOutputDataKind(bool*)` (`NativeEffectBase.h:253`), and a policy that reads a knob must call `Node::invalidateEffectiveOutputDataKindCache()` itself (`NativeEffectBase.h:~244`). Declare NativeRead's output polymorphic and override `resolveOutputDataKind` to return Raw when the filename's extension is in the RAW set (P2.T3) and Image otherwise, never ambiguous. Invalidate on `filename` changes. Downstream edges whose kind flips go through the existing conflict reporting and are never disconnected. A source node with a knob-driven kind is new, so document it in the README.
  - verify: gtest: a Read whose filename moves PNG→RAW→PNG flips Image→Raw→Image; a raw-only consumer reports a conflict when it flips back; a project save/load round trip keeps the resolved kind (`Tests/DataKindProjectLoad_Test.cpp` pattern).
  - size: M

- [ ] M80.P2.T3 — Read renders raw for RAW files
  - files: `Engine/Nodes/IO/NativeRead.cpp`, `Engine/Nodes/IO/OiioReadSupport.cpp` (extension claim only), `Engine/Nodes/IO/docs/NativeRead.md`, new `Tests/Native/NativeReadRaw_Test.cpp`
  - approach: the native Read claims a constant set of rawspeed's supported extensions (cr2, cr3, nef, arw, raf, rw2, orf, dng, pef, srw, and the rest rawspeed lists) before `readableExtensions()` (`OiioReadSupport.h:302`). OIIO's `raw` stays in `kExcludedOiioFormats` (`OiioReadSupport.cpp:815`). For a RAW file the Read outputs `eDataKindRaw`: the decoded channels, the active-area window as the RoD, data from `RawDecodeSupport`, and the `RawMetadata` keys merged into the file metadata (`deriveOutputMetadata`). `ocioInputSpace` does not apply to raw data, so hide the colour knobs for a raw file. Frame sequences and the time domain work as for images. Proxy is disabled for raw, because downscaling a mosaic is meaningless; the tooltip says so. An unsupported camera sets a persistent error with P2.T1's text. Replace the "RAW is excluded; a later milestone adds it" line in `NativeRead.md:87`.
  - verify: gtest: a Read of each fixture is raw kind with the expected channel count, metadata keys and values; a non-RAW file still outputs an image; the unsupported-camera path sets the persistent error.
  - size: M

- [ ] M80.P2.T4 — The Viewer accepts raw directly
  - files: `Engine/ViewerInstance.cpp/.h`, new `Engine/Nodes/Raw/RawDisplay.h/.cpp`, `Engine/Nodes/README.md`
  - approach: extend the Viewer's adapter exception (`ViewerInstance.h`, `inputAcceptsDataKindViaAdapter`, which returns `kind == eDataKindDeep` today) to Raw. `isRegisteredAdapter` (`Node.cpp:~1895`) hard-codes Deep→Image; make the Raw acceptance Viewer-only (no general Raw→Image adapter row, per P1.T2), choosing the smallest mechanism that keeps other image inputs rejecting raw. Display goes through a shared `RawDisplay` conversion: a mosaic becomes a greyscale single channel, and linear raw shows its channels as they are. RawToImage (P2.T5) reuses the same function. No demosaic in the viewer. The info line shows "RAW mosaic" or "RAW linear". Update the README's Viewer-exception paragraph.
  - verify: gtest: a viewer fed by a raw Read renders without error, and its pixel values equal the mosaic; a linear DNG shows three channels. Extend the `Tests/gui` smoke script to show it.
  - size: M

- [ ] M80.P2.T5 — RawToImage node
  - files: new `Engine/Nodes/Raw/RawToImage.h/.cpp`, `RawToImage.reg.inc`, `docs/RawToImage.md`, new `Tests/Native/NativeRawToImage_Test.cpp`
  - approach: raw in, image out, no knobs beyond the framework's. A plain conversion through `RawDisplay` (P2.T4): mosaic → a 1-channel grey image (alpha 1), linear → channels as they are. No scaling, no colour. It is the explicit converter for users who want raw values in an image graph. Tab menu `Raw/RawToImage` via `NodeSpec::grouping`.
  - verify: gtest: output equals the input values for a mosaic and a linear fixture; output kind is image; `expectNativeIdentity` passes.
  - size: M

## Phase 80.3: Core develop chain (CPU)

Each port names the darktable source file in its `approach` as the reference. Ports take the C path at `DARKTABLE_REF` (P1.T1) and never the SSE or OpenCL variants; core-chain GPU kernels are Phase 80.6. Ports keep the darktable copyright header. Every node is a framework node (P1.T4) under `Engine/Nodes/Raw/`. Per node, it adds `<Node>.reg.inc`, `<Node>.costseed.inc`, `docs/<Node>.md`, an icon and a `Tests/Native/Native<Node>_Test.cpp` calling `expectNativeIdentity`, per the README checklist; no shared file is edited. Per-pixel mosaic work is a Point op that uses `RowIO::x0/y`. Neighbourhood work is a Passes op, or Custom where the algorithm's structure needs it. Each node declares raw in and raw out unless stated, and checks `raw/stage`. Prepare, Temperature and Highlights accept 1 or 3 channels (linear raw). Nodes sit in the Tab menu under `Raw` (`NodeSpec::grouping`).

- [ ] M80.P3.T1 — RawPrepare node (levels, scaling, crop)
  - files: new `Engine/Nodes/Raw/RawPrepare.h/.cpp` + per-node files, new `Tests/Native/NativeRawPrepare_Test.cpp`
  - approach: port darktable `src/iop/rawprepare.c`. Subtract per-CFA-colour black levels (per channel for linear), scale by the white level to 0-1, apply the crop (shifting the pattern origin via `RawLayout`), and set `raw/stage` to `prepared`. A `prepared` input passes through with a warning. Knobs: black levels (override), white level (override), crop margins. Defaults come from the metadata.
  - verify: gtest against a hand-computed mosaic with known levels; a crop by an odd number of pixels keeps `colourAt` correct; a linear 3-channel input scales per channel; a prepared input passes through unchanged.
  - size: M

- [ ] M80.P3.T2 — Temperature node (white balance on raw)
  - files: new `Engine/Nodes/Raw/RawTemperature.h/.cpp` + per-node files, new `Tests/Native/NativeRawTemperature_Test.cpp`
  - approach: port the raw-domain part of darktable `src/iop/temperature.c`: per-CFA-colour gains on the prepared mosaic, or per-channel gains on linear data. Knobs: temperature, tint, the three channel coefficients, and a mode (as shot, camera reference, custom). The temperature-to-coefficient conversion uses the camera matrix from metadata, as darktable does. Port the `dt_colorspaces_...` helpers it needs locally rather than pulling in darktable's colour library. It requires `raw/stage` = `prepared`.
  - verify: gtest: as-shot coefficients reproduce rawspeed's; a custom temperature on a synthetic grey patch with a known matrix gives equal channels after demosaic (checked once P3.T4 lands); an unprepared input raises the stage error.
  - size: M

- [ ] M80.P3.T3 — Highlights node (clip and reconstruct)
  - files: new `Engine/Nodes/Raw/RawHighlights.h/.cpp` + per-node files, new `Tests/Native/NativeRawHighlights_Test.cpp`
  - approach: port darktable `src/iop/highlights.c`. Modes: clip, unclipped (leave), and reconstruction ("inpaint opposed" at `DARKTABLE_REF`; LCh where the pattern allows). The clip threshold needs the white level and white-balance coefficients from metadata.
  - verify: gtest: a synthetic mosaic with a clipped patch, each mode matching a reference computed from darktable on the same input. The reference is generated once by the P5.T1 harness and stored as a small fixture; the fixture README says how.
  - size: M

- [ ] M80.P3.T4 — Demosaic node: PPG and RCD (Bayer), linear pass-through
  - files: new `Engine/Nodes/Raw/RawDemosaic.h/.cpp` + per-node files, new `Engine/Nodes/Raw/DemosaicBayer.h/.cpp`, new `Tests/Native/NativeRawDemosaicBayer_Test.cpp`
  - approach: port darktable `src/iop/demosaic.c` and `src/iop/demosaicing/ppg.c` and `rcd.c`. Raw in, image out: three colour channels plus alpha 1. Input with no CFA (linear) passes through as image with alpha 1. The halo comes from the algorithm (RCD needs 4-6 px), rounded up to the pattern period, and the RoD equals the input's. Knobs: method, green equilibration, colour smoothing passes. The method knob is an enum, so P3.T5 and P3.T6 add choices.
  - verify: gtest: a synthetic gradient mosaic reconstructs within a bound; a tile rendered with RoI expansion equals the same pixels of the full-frame render, bit for bit; output kind is image; a linear fixture passes through unchanged.
  - size: L

- [ ] M80.P3.T5 — Demosaic: AMaZE
  - files: new `Engine/Nodes/Raw/DemosaicAmaze.h/.cpp`, `Engine/Nodes/Raw/RawDemosaic.cpp` (method enum), new `Tests/Native/RawDemosaicAmaze_Test.cpp`
  - approach: port darktable `src/iop/demosaicing/amaze.cc` (GPL-3). It works in 128-px tiles with its own overlap. Map that onto Natron's RoI (an 18 px halo, aligned to the pattern), not onto a whole-image buffer.
  - verify: gtest: tiled equals full-frame on a fixture; the result is within a bound of PPG's on a smooth patch and measurably sharper on an edge pattern.
  - size: L

- [ ] M80.P3.T6 — Demosaic: X-Trans (Markesteijn)
  - files: new `Engine/Nodes/Raw/DemosaicXTrans.h/.cpp`, `Engine/Nodes/Raw/RawDemosaic.cpp`, new `Tests/Native/RawDemosaicXTrans_Test.cpp`
  - approach: port darktable `src/iop/demosaicing/xtrans.c`: 1-pass and 3-pass Markesteijn, plus the fast VNG variant for X-Trans previews. The 6x6 pattern period drives the halo alignment.
  - verify: gtest: on the X-Trans fixture, tiled equals full-frame; the output has no pattern artefacts on a flat patch.
  - size: L

- [ ] M80.P3.T7 — Input colour node (camera to working space)
  - files: new `Engine/Nodes/Raw/RawInputColor.h/.cpp` + per-node files, `Engine/Project.cpp/.h` (only if a working-space accessor is missing), new `Tests/Native/NativeRawInputColor_Test.cpp`
  - approach: image in, image out, placed after demosaic: a Point op. Camera RGB goes to XYZ D65 through the metadata matrix (`raw/cam_to_xyz`), which the user can override; an ICC/DCP-less "standard matrix" is the fallback. Then XYZ goes to the project working space through the project OCIO (`DESIGN/2026-10-02-project-ocio.md`): build an OCIO matrix transform to the working space rather than hard-coding primaries. Port the matrix handling from darktable `src/iop/colorin.c`, not its ICC profile path.
  - verify: gtest: a grey-card mosaic with the metadata matrix ends neutral in the working space; changing the project working space re-renders.
  - size: M

- [ ] M80.P3.T8 — Develop RAW: auto-insert the chain on interactive create
  - files: new `Engine/Nodes/Raw/RawDevelop.h/.cpp` (chain builder), the GUI node-creation hook found while scouting (Tab-menu create, file drop and file-browser create paths in `Gui/`), `Engine/Settings.cpp` (preference), `Tests/gui/m80_raw.py`
  - approach: when the user interactively creates a Read on a RAW file, insert RawPrepare → Temperature → Highlights → Demosaic → InputColor below it with darktable's defaults, as **loose nodes inside a backdrop** titled "Develop RAW". Groups are an image barrier (README "Known limitation"), and the backdrop is the permanent form. A "Develop RAW files on read" preference turns it off; it defaults on. It never fires on project load, Python `createNode`, or a filename change on an existing Read. Scout the creation paths first and put the hook at the single point they share; report it if no such point exists. The chain builder lives in Engine so tests can call it.
  - verify: gtest calls the chain builder on a fixture Read and checks the five nodes, their order and the backdrop. The Xvfb script creates a Read on a fixture through the GUI path and sees the chain and a plausible colour image in the viewer; loading a saved project does not add a second chain; ctest is otherwise unchanged.
  - size: M

## Phase 80.4: Extra modules

- [ ] M80.P4.T1 — Hot pixels node
  - files: new `Engine/Nodes/Raw/RawHotPixels.h/.cpp` + per-node files, new `Tests/Native/NativeRawHotPixels_Test.cpp`
  - approach: port darktable `src/iop/hotpixels.c`: threshold and strength, running on the prepared mosaic with a 2-px halo (Passes).
  - verify: gtest: injected hot pixels on a smooth mosaic are removed, and a clean mosaic comes back bit-identical.
  - size: M

- [ ] M80.P4.T2 — Chromatic aberration node
  - files: new `Engine/Nodes/Raw/RawCACorrect.h/.cpp` + per-node files, new `Tests/Native/NativeRawCACorrect_Test.cpp`
  - approach: port darktable `src/iop/cacorrect.c` (Bayer; X-Trans passes through with a message). It estimates shifts over the whole image, so it needs the full frame. Cache the shift estimate per (input hash, parameters) instead of recomputing it per tile, and have the RoI request the full input for estimation (Custom op).
  - verify: gtest: a synthetic mosaic with a known channel shift is corrected within a bound; tiled equals full-frame.
  - size: L

- [ ] M80.P4.T3 — Raw denoise node
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

- [ ] M80.P5.T1 — Reference comparison harness
  - files: new `Tests/Native/RawReference_Test.cpp`, new `tools/raw/make-reference.sh`, `Tests/fixtures/raw/README.md`
  - approach: a script builds darktable at `DARKTABLE_REF` in a throwaway container. It develops each fixture with a fixed XMP sidecar (only the ported modules enabled, no tone mapping) to a 32-bit TIFF in the same working space, and the TIFF is committed as a small fixture. The test runs the Develop chain on the same file and compares with a tolerance per method (PPG and RCD close, AMaZE and Markesteijn within a looser bound), reporting mean and max error. If the sandbox can't pull the base image, generate the references on a host that can and commit them.
  - verify: the test passes for the Bayer and X-Trans fixtures; the README states the tolerances and why.
  - size: M

- [ ] M80.P5.T2 — Large-image and threading check
  - files: new `Tests/Native/RawLarge_Test.cpp`, `tools/bench/`
  - approach: a synthetic 100 MP mosaic, generated at run time and never committed, runs through the whole chain. Check for no `int` overflow (`size_t` row arithmetic), memory bounded by the decode cache cap, and scaling across threads without a lock convoy (check with the `tools/bench` scripts, per the render-scaling findings). Concurrent renders of two views of one file share one decode.
  - verify: the gtest completes under a stated memory bound; the bench shows time falling with thread count.
  - size: M

- [ ] M80.P5.T3 — Panels and viewer check under Xvfb
  - files: `Tests/gui/m80_raw.py`, node panel layouts in `Engine/Nodes/Raw/*.cpp` (labels and pages only)
  - approach: run the Xvfb recipe (`build/deeprepro/run-gui.sh`, with `checkForUpdates` pre-seeded off). Open a Bayer, an X-Trans and a linear-DNG fixture, check the auto-inserted Develop backdrop, switch demosaic methods, change temperature, view a raw edge directly and through RawToImage. Capture screenshots of each node's panel and the viewer. Share the screenshots and get the user's approval before sign-off (project rule).
  - verify: the scripts pass under Xvfb; the screenshots are shared and the user approves them.
  - size: M

## Phase 80.6: Core-chain GPU kernels

Kernels are Slang under `Engine/Gpu/Kernels/`, each with its own `<kernel>.cmake` fragment (`slang_add_kernel`). Point ops return `GpuOpRef::compiled` from `gpu()`, with the C++ maths mirrored in Slang and a `static_assert` on the parameter-block size (README "The GPU side of an op"). Passes ops use `PassList::gpu`. Ports start from darktable's OpenCL at `DARKTABLE_REF`. Parity follows README "GPU parity". CPU stays the default and the reference, and a machine with no usable device falls back silently.

- [ ] ~~M80.P6.T1 — Raw-kernel requirements for the engine GPU backend~~ (cancelled 2026-10-22: M83 - GPU Compute Backend Spike settled Vulkan compute + Slang, and core-chain kernels now ship in this phase)
  - files: —
  - approach: —
  - verify: —
  - size: —

- [ ] M80.P6.T2 — Raw data in GPU regions
  - files: `Engine/Gpu/GpuBuffer.*`, `Engine/Gpu/GpuWrap.*`, `Engine/GpuPlacement.*`, `Engine/Nodes/Image/StagedImageEffect.cpp` (raw-in GPU path), new `Tests/Gpu/RawRegionGpu_Test.cpp`
  - approach: let a raw-kind region (1 or 3 channels) live resident on the GPU like an image region, and give kernels the `RawLayout` (pattern period, origin, the 6x6 colour table) as a uniform block. Placement treats raw-kind nodes as eligible only when every op in the run has a GPU path. Scout first whether `GpuBuffer`/`GpuWrap` already handle one channel; report if this needs more than bounded changes.
  - verify: a 1-channel mosaic uploads and reads back bit-exactly; a test kernel reading `colourAt` from the uniform matches the CPU `RawLayout` on every pixel of a Bayer and an X-Trans layout.
  - size: L

- [ ] M80.P6.T3 — Prepare and Temperature kernels
  - files: new `Engine/Gpu/Kernels/rawprepare.slang`, `rawprepare.cmake`, `Engine/Nodes/Raw/RawPrepare.cpp`, `Engine/Nodes/Raw/RawTemperature.cpp`, new `Tests/Native/NativeRawPrepTempGpu_Test.cpp`
  - approach: one Slang module with two `IPixelOp`s (levels/scale/crop-offset, and per-CFA-colour gains), from darktable's `basic.cl` rawprepare and whitebalance kernels. Wire each node's `gpu()`.
  - verify: GPU parity against the CPU nodes on the Bayer, X-Trans and linear fixtures, within the README's parity tolerance.
  - size: M

- [ ] M80.P6.T4 — Highlights kernels
  - files: new `Engine/Gpu/Kernels/rawhighlights.slang`, `rawhighlights.cmake`, `Engine/Nodes/Raw/RawHighlights.cpp`, new `Tests/Native/NativeRawHighlightsGpu_Test.cpp`
  - approach: port darktable's highlights OpenCL (clip, and the reconstruction mode P3.T3 ported). Clip is a Point kernel; reconstruction follows the CPU op's pass structure.
  - verify: GPU parity against the CPU node for each mode on the clipped-patch fixture.
  - size: M

- [ ] M80.P6.T5 — Demosaic PPG and RCD kernels
  - files: new `Engine/Gpu/Kernels/demosaic_bayer.slang`, `demosaic_bayer.cmake`, `Engine/Nodes/Raw/RawDemosaic.cpp`, `Engine/Nodes/Raw/DemosaicBayer.cpp`, new `Tests/Native/NativeRawDemosaicGpu_Test.cpp`
  - approach: port darktable `demosaic_ppg.cl` and `demosaic_rcd.cl` (multi-pass, local memory) as `PassList::gpu` passes. AMaZE, Markesteijn and VNG report no GPU path, so a chain using them renders on the CPU (M81 - Raw GPU Kernels). Linear pass-through needs no kernel.
  - verify: GPU parity against the CPU node for PPG and RCD on the Bayer fixture; the tiled GPU render equals the full-frame GPU render.
  - size: L

- [ ] M80.P6.T6 — InputColor kernel and resident-chain check
  - files: new `Engine/Gpu/Kernels/rawinputcolor.slang`, `rawinputcolor.cmake`, `Engine/Nodes/Raw/RawInputColor.cpp`, new `Tests/Native/NativeRawChainGpu_Test.cpp`
  - approach: a 3x3 camera→working-space matrix Point kernel (the OCIO matrix baked to the node's parameter block). Then check that the whole Develop chain, Prepare through InputColor with RCD, runs as one resident GPU region with no readback between nodes.
  - verify: GPU parity for InputColor; the chain test shows the end-to-end GPU result within tolerance of the CPU chain and a single upload/readback (from the placement trace).
  - size: M

**Verification gate:**
- The GUI bench rule applies only once M117 - GUI Perf Harness And Baseline has landed; until then record "N/A, no bench yet" in `## Decisions`.
- `ctest` is green, including every new `Tests/Native/*Raw*`, `NativeReadRaw`, `NativeSourceKind`, `RawKindFramework` and core-chain GPU parity test. The GPU tests run on RADV/llvmpipe as the existing GPU tests do.
- The darktable reference comparison (M80.P5.T1) passes within its stated tolerances on a Bayer and an X-Trans fixture.
- A linear DNG reads as raw and develops.
- An unsupported-camera stub shows the named error.
- The large-image test (M80.P5.T2) passes.
- The Xvfb script (M80.P5.T3) passes, and the user has approved the screenshots.

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
