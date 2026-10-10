# M80 - Native RAW Support

Brings RAW camera files back as a native pipeline ported from darktable: rawspeed decodes the file, the native Read emits the undemosaiced sensor mosaic on a new `eDataKindRaw` edge, and native nodes ported from darktable's CPU modules (rawprepare, temperature, highlights, demosaic, input colour, plus hot pixels, chromatic aberrations, raw denoise and lens correction) turn it into a scene-linear image. This supersedes the "RAW camera formats are dropped" clause of `DECISIONS/2026-10-09-native-read-format-scope.md`; the "different technique, not OIIO's libraw plugin" it anticipated is rawspeed plus these nodes. GPU kernels are out of this milestone: Phase 80.6 only decides the backend, and M81 - Raw GPU Kernels does the ports. Licensing: darktable's modules are GPL-3.0-or-later, rawspeed is LGPL-2.1, and Natron is GPL-2-or-later, so ported files keep their darktable copyright headers and the combined work is distributed under GPL-3 terms; record this in `tools/license/components/README.md`. Depends on M75 - Native Read shipping (the reader structure) and on M74 - Native Metadata Core (the mosaic parameters travel as image metadata).

## Phase 80.1: Foundation

- [ ] M80.P1.T1 — Vendor rawspeed as a pinned dependency
  - files: `tools/ci/local/fetch-assets.sh`, `Engine/CMakeLists.txt`, `tools/license/components/README.md`, `tools/release/stage-bundle.sh` (only if the shared object needs bundling)
  - approach: build rawspeed (and its pugixml dependency, if the image lacks it) from source at an exact commit SHA, following the `LCMS2_REPO`/`LCMS2_REF` recipe (`fetch-assets.sh:173-174,286-330`) into `build/assets/plugin-src/deps-install/`; static if rawspeed supports it, so the AppImage needs nothing new. `find_package` or pkg-config it in `Engine/CMakeLists.txt` and link it. Add the licence entries (rawspeed LGPL-2.1, pugixml MIT, the darktable GPL-3 note). Verify the dev image first: `docker ps`, then check what the image already ships before building anything. Include rawspeed's `cameras.xml` in the bundle; the decoder needs it at run time.
  - verify: a one-line test binary links rawspeed and `RawDecoder` constructs; the fetch step is idempotent on a second run; the licence README names both libraries.
  - size: M

- [ ] M80.P1.T2 — Add the `eDataKindRaw` data kind and its Raw-to-Image adapter row
  - files: `Global/Enums.h`, `Engine/Node.cpp` (kind switches, `isRegisteredAdapter` at ~1026), `Gui/Edge.cpp`, `Gui/NodeGui.cpp`, `Engine/Nodes/README.md`
  - approach: add `eDataKindRaw` to `DataKindEnum` (`Global/Enums.h:514`). The payload is a one-channel float `Image` holding the CFA mosaic in the existing image cache, so no new render virtual or cache (unlike Deep's `EffectInstanceRenderDeep.cpp` path); the kind exists for edge typing only. Add the adapter row so a raw output can feed an Image input only through a node that declares raw in and image out (demosaic), not implicitly. Colour the edge and the node tint with a new Okabe-Ito colour distinct from Deep's blue (`Gui/Edge.cpp:788-811`, `Gui/NodeGui.cpp:189-196`). Check `Engine/EffectInstance.cpp:5210`, `Engine/OutputSchedulerThread.cpp` (2402-2648) and `Engine/ViewerInstance.h:454` for switches that need a raw arm. Document the kind in the README "Data kinds" section, including that the mosaic's layout travels as metadata (P1.T3).
  - verify: gtest: a raw output cannot connect to an image-only input (`eCanConnectInput_incompatibleDataKind`) and can connect to a raw input; a project with a conflicting edge reports it on load via `reportDataKindConflicts`; full ctest unchanged.
  - size: M

- [ ] M80.P1.T3 — Define the raw mosaic metadata schema
  - files: new `Engine/Nodes/IO/RawMetadata.h/.cpp`, new `Tests/Native/RawMetadata_Test.cpp`
  - approach: typed accessors over the M74 metadata store (`Engine/Nodes/Metadata/ImageMetadata.h`) for the keys every raw node needs: `raw/cfa_pattern` (2x2 Bayer or 6x6 X-Trans as a string plus the pattern origin offset), `raw/black_levels` (per CFA colour), `raw/white_level`, `raw/wb_coeffs` (as shot), `raw/cam_to_xyz` (3x3 D65 matrix from rawspeed's camera database), `raw/crop` (active area), `raw/iso`, `raw/exposure`, `raw/stage` (`raw`, `prepared`: scaled to 0-1 with black subtracted). Provide `RawLayout` (pattern period, colour-at(x, y) with the crop offset applied) so every node shares one definition. A node that crops or shifts the window must update the pattern origin; document that.
  - verify: gtest: round-trip every key; `colourAt` matches a hand table for RGGB, BGGR, GRBG, GBRG and a 6x6 X-Trans, including after a one-pixel crop offset.
  - size: M

## Phase 80.2: Decode and read

- [ ] M80.P2.T1 — rawspeed decode support
  - files: new `Engine/Nodes/IO/RawDecodeSupport.h/.cpp`, new `Tests/Native/RawDecodeSupport_Test.cpp`, new fixtures under `Tests/fixtures/raw/`
  - approach: model it on `OiioReadSupport` (P1.T2 of M75 - Native Read): a header cache keyed on (path, mtime, size) holding the parsed `RawImage` metadata, behind a mutex, and `decode(path, RectI window, float* dst, stride)` copying the window of the mosaic as float. rawspeed decodes whole files, so keep one decoded `RawImage` per cache key with a size-bounded LRU (many-core, high-RAM and modest machines alike: the cap is a fraction of the project cache, not a constant). Fill `RawMetadata` from the decoder (CFA, black/white levels, as-shot white balance, camera matrix from `cameras.xml`, crop). All offsets `size_t`. Fixtures: two or three small CC0 raw files (a Bayer DNG, an X-Trans RAF or similar) from the public raw.pixls.us samples; record their source in `Tests/fixtures/raw/README.md`.
  - verify: gtest: dimensions, CFA pattern, levels and crop match rawspeed's own output for each fixture; a window decode equals the same window of the full decode bit for bit; a truncated file fails cleanly.
  - size: M

- [ ] M80.P2.T2 — Let a native Read resolve its output kind from the file
  - files: `Engine/Nodes/NativeEffectBase.h/.cpp`, `Engine/Node.cpp` (kind resolution ~1051-1152), `Engine/Nodes/README.md`, new `Tests/Native/NativeSourceKind_Test.cpp`
  - approach: kinds are static plugin declarations today; `resolveOutputDataKind` (`NativeEffectBase.h:297`) exists for polymorphic nodes and keys off inputs. Read has no input and is `eDataKindImage`. Scout first and report: the least invasive way for a source node to declare a polymorphic output whose kind comes from a knob-derived value (the file extension), including cache invalidation when `filename` changes (`Node.cpp:~1363`) and what happens to downstream edges whose kind flips (they must go through the existing conflict reporting, not be disconnected). Implement that. If it cannot be done without changing every node, stop and report as an open question: the fallback is a separate `RawRead` node, and the user chose Read-emits-mosaic.
  - verify: gtest: a test source node whose kind follows a string knob flips Image to Raw; the downstream raw-only node reports a conflict when it flips back; a project save/load round trip keeps the resolved kind.
  - size: L

- [ ] M80.P2.T3 — Read renders the mosaic for RAW files
  - files: `Engine/Nodes/IO/NativeRead.cpp`, `Engine/Nodes/IO/OiioReadSupport.cpp` (extension list only), `Engine/AppManager.cpp` (RAW extension set), new `Tests/Native/NativeReadRaw_Test.cpp`
  - approach: a constant set of rawspeed's supported extensions (cr2, cr3, nef, arw, raf, rw2, orf, dng, pef, srw and the rest rawspeed lists) is claimed by the native Read before OIIO's `extension_list`, which still must not hand them to its libraw plugin. For a RAW file the Read outputs `eDataKindRaw`: one float channel, the active-area window as the RoD, the mosaic from `RawDecodeSupport`, and the `RawMetadata` keys merged into the file metadata (`deriveOutputMetadata`). `ocioInputSpace` is not applied to mosaic data: hide the colour knobs for a raw file. Frame sequences, proxy and the time domain work as for images (the proxy is disabled for raw: downscaling a mosaic is meaningless; say so in the tooltip).
  - verify: gtest: Read of each fixture is raw-kind, one channel, with the expected metadata keys and mosaic values; a non-RAW file still outputs an image; switching `filename` between a PNG and a RAW file changes the kind and the downstream edge conflict is reported.
  - size: M

- [ ] M80.P2.T4 — View the mosaic directly
  - files: `Engine/ViewerInstance.cpp/.h` (`:454-459` region), `Engine/Nodes/README.md`
  - approach: let the viewer accept a raw-kind input and display it as a greyscale single channel, by treating the mosaic as a one-channel image (the payload already is one). No demosaic in the viewer. Show "RAW mosaic" in the viewer's info line.
  - verify: gtest: a viewer connected to a raw Read renders without error and the pixel values equal the mosaic; the `Tests/gui` smoke script is extended to show it.
  - size: M

## Phase 80.3: Core develop chain (CPU)

Each port keeps the darktable source file named in `approach` as the reference, ports the C path only (never the SSE/OpenCL variants; those return in M81 - Raw GPU Kernels), and keeps the darktable copyright header. Every node declares raw in and raw out unless stated, uses the row-band helpers per the Nodes README "Threading" rules, and copies `Blur`'s RoI pattern (`getSourceRoI`, `getRegionsOfInterest`) where it needs neighbours. Nodes register in the Tab menu under a `Raw` group.

- [ ] M80.P3.T1 — RawPrepare node (levels, scaling, crop)
  - files: new `Engine/Nodes/Raw/RawPrepare.h/.cpp`, `Engine/AppManager.cpp` (one registration), new `Tests/Native/RawPrepare_Test.cpp`
  - approach: port darktable `src/iop/rawprepare.c` (C path). Subtract per-CFA-colour black levels, scale by the white level to 0-1, apply the crop (shifting the pattern origin via `RawLayout`), set `raw/stage` to `prepared`. Knobs: black levels (override), white level (override), crop margins. Defaults come from the metadata.
  - verify: gtest against a hand-computed mosaic with known levels; a crop by an odd number of pixels keeps `colourAt` correct.
  - size: M

- [ ] M80.P3.T2 — Temperature node (white balance on the mosaic)
  - files: new `Engine/Nodes/Raw/RawTemperature.h/.cpp`, registration, new `Tests/Native/RawTemperature_Test.cpp`
  - approach: port the raw-domain part of darktable `src/iop/temperature.c`: per-CFA-colour gains applied to the prepared mosaic. Knobs: temperature, tint, the three channel coefficients, and a mode (as shot, camera reference, custom). The temperature-to-coefficient conversion uses the camera matrix from metadata, as darktable does; port `dt_colorspaces_...` helpers it needs locally rather than pulling darktable's colour library.
  - verify: gtest: as-shot coefficients reproduce rawspeed's; a custom temperature on a synthetic grey patch with a known matrix gives equal demosaiced channels (checked after P3.T4).
  - size: M

- [ ] M80.P3.T3 — Highlights node (clip and reconstruct)
  - files: new `Engine/Nodes/Raw/RawHighlights.h/.cpp`, registration, new `Tests/Native/RawHighlights_Test.cpp`
  - approach: port darktable `src/iop/highlights.c` C path: clip, unclipped (leave), and LCh reconstruction where the mosaic allows (the "inpaint opposed" mode of newer darktable if the source tree has it; pin the darktable commit this task ports from in the file header). Needs the white level and white-balance coefficients from metadata for the clip threshold.
  - verify: gtest: a synthetic mosaic with a clipped patch, each mode matching a reference computed from the ported algorithm on the same input (reference generated once from darktable built in a throwaway container and stored as a small fixture; record how in the fixture README).
  - size: M

- [ ] M80.P3.T4 — Demosaic node: PPG and RCD (Bayer)
  - files: new `Engine/Nodes/Raw/RawDemosaic.h/.cpp`, new `Engine/Nodes/Raw/DemosaicBayer.h/.cpp`, registration, new `Tests/Native/RawDemosaicBayer_Test.cpp`
  - approach: port darktable `src/iop/demosaic.c` and `src/iop/demosaicing/ppg.c` and `rcd.c`. Raw in, image out (the registered adapter from M80.P1.T2): three colour channels plus alpha 1. Halo from the algorithm (RCD needs 4-6 px) rounded up to the pattern period; the RoD equals the input's. Knobs: method, green-equilibration, colour smoothing passes. The demosaic node is the only node that does this; its method knob is an enum so P3.T5 and T6 add choices.
  - verify: gtest: a synthetic gradient mosaic reconstructs within a bound; a tile rendered with RoI expansion equals the same pixels of the full-frame render bit for bit (tiling safety); output kind is image.
  - size: L

- [ ] M80.P3.T5 — Demosaic: AMaZE
  - files: `Engine/Nodes/Raw/DemosaicAmaze.h/.cpp`, `Engine/Nodes/Raw/RawDemosaic.cpp` (method enum), new `Tests/Native/RawDemosaicAmaze_Test.cpp`
  - approach: port darktable `src/iop/demosaicing/amaze.cc` (GPL-3). It works in 128-px tiles with its own overlap: map that onto Natron's RoI (halo 18 px, pattern-aligned), not onto a whole-image buffer.
  - verify: gtest: tiled equals full-frame on a fixture; result within a bound of PPG's on a smooth patch and measurably sharper on an edge pattern.
  - size: L

- [ ] M80.P3.T6 — Demosaic: X-Trans (Markesteijn)
  - files: new `Engine/Nodes/Raw/DemosaicXTrans.h/.cpp`, `Engine/Nodes/Raw/RawDemosaic.cpp`, new `Tests/Native/RawDemosaicXTrans_Test.cpp`
  - approach: port darktable `src/iop/demosaicing/xtrans.c` (1-pass and 3-pass Markesteijn) and the fast VNG variant for X-Trans previews. The 6x6 pattern period drives the halo alignment.
  - verify: gtest: X-Trans fixture, tiled equals full-frame; output has no pattern artefacts on a flat patch.
  - size: L

- [ ] M80.P3.T7 — Input colour node (camera to working space)
  - files: new `Engine/Nodes/Raw/RawInputColor.h/.cpp`, `Engine/Project.cpp/.h` (only if a working-space accessor is missing), registration, new `Tests/Native/RawInputColor_Test.cpp`
  - approach: image in, image out, placed after demosaic. Camera RGB to XYZ D65 through the metadata matrix (`raw/cam_to_xyz`, user-overridable, plus an optional ICC/DCP-less "standard matrix" fallback), then to the project working space through the project OCIO (`DESIGN/2026-10-02-project-ocio.md`): build an OCIO matrix transform to the working space rather than hard-coding primaries. Port the matrix handling from darktable `src/iop/colorin.c`; do not port its ICC profile path.
  - verify: gtest: a grey card mosaic with the metadata matrix ends neutral in the working space; changing the project working space re-renders.
  - size: M

- [ ] M80.P3.T8 — RawDevelop preset: Read to a gradable image in one step
  - files: `Engine/AppInstance.cpp` or the node-creation hook found while scouting, `Engine/Nodes/Raw/RawDevelop.cpp` (or a Python gizmo under the existing gizmo mechanism: choose after scouting), `Tests/gui/m80_raw.py`
  - approach: since Read emits the mosaic, add a one-action "Develop RAW" that inserts RawPrepare, Temperature, Highlights, Demosaic and InputColor after a raw Read with the darktable defaults, as a group. It is a convenience: each node stays individually usable. Prefer a user-visible Group over a hidden composite node so the user can open and edit it.
  - verify: the Xvfb script creates a Read on a fixture, runs Develop, and the viewer shows a plausible colour image; ctest unchanged.
  - size: M

## Phase 80.4: Extra modules

- [ ] M80.P4.T1 — Hot pixels node
  - files: new `Engine/Nodes/Raw/RawHotPixels.h/.cpp`, registration, new `Tests/Native/RawHotPixels_Test.cpp`
  - approach: port darktable `src/iop/hotpixels.c`: threshold and strength, runs on the prepared mosaic with a 2-px halo.
  - verify: gtest: injected hot pixels on a smooth mosaic are removed and a clean mosaic is bit-identical.
  - size: M

- [ ] M80.P4.T2 — Chromatic aberration node
  - files: new `Engine/Nodes/Raw/RawCACorrect.h/.cpp`, registration, new `Tests/Native/RawCACorrect_Test.cpp`
  - approach: port darktable `src/iop/cacorrect.c` (Bayer; X-Trans passes through with a message). It estimates shifts over the whole image, so it needs the full frame: cache the shift estimate per (input hash, parameters) instead of recomputing per tile, and make the RoI request the full input for estimation.
  - verify: gtest: a synthetic mosaic with a known channel shift is corrected within a bound; tiled equals full-frame.
  - size: L

- [ ] M80.P4.T3 — Raw denoise node
  - files: new `Engine/Nodes/Raw/RawDenoise.h/.cpp`, bundled noise-profile data under `Engine/Nodes/Raw/data/`, registration, new `Tests/Native/RawDenoise_Test.cpp`
  - approach: port darktable `src/iop/rawdenoise.c` (wavelet) and the profile-driven `denoiseprofile.c` non-local-means variant; start with profile data for the fixture cameras and read the rest from darktable's `noiseprofiles.json` (record the pinned copy and licence). Knobs: method, threshold or strength, ISO override.
  - verify: gtest: added Gaussian noise on a smooth mosaic is reduced measurably (PSNR improves); edges preserved within a bound.
  - size: L

- [ ] M80.P4.T4 — Lens correction: add the lensfun dependency
  - files: `tools/ci/local/fetch-assets.sh`, `Engine/CMakeLists.txt`, `tools/license/components/README.md`
  - approach: pin and build lensfun (LGPL-3 library, database CC-BY-SA) the same way as P1.T1 and bundle its database. Check the image for an existing lensfun first.
  - verify: a test binary opens the lensfun database and finds a known lens entry.
  - size: M

- [ ] M80.P4.T5 — Lens correction node
  - files: new `Engine/Nodes/Raw/RawLensCorrect.h/.cpp`, registration, new `Tests/Native/RawLensCorrect_Test.cpp`
  - approach: port darktable `src/iop/lens.cc`: distortion, vignetting and transverse chromatic aberration from the lensfun profile, with the camera and lens read from `raw/*` metadata (add the make, model and lens keys to `RawMetadata` in this task) and manual override knobs. Image in, image out, placed after demosaic. Distortion changes the geometry: implement `getRegionOfDefinition` and `getRegionsOfInterest` from the inverse mapping, with a bounded halo.
  - verify: gtest: a synthetic grid through a known distortion profile lands within a sub-pixel bound; tiled equals full-frame.
  - size: L

## Phase 80.5: Verification against darktable

- [ ] M80.P5.T1 — Reference comparison harness
  - files: new `Tests/Native/RawReference_Test.cpp`, new `tools/raw/make-reference.sh`, `Tests/fixtures/raw/README.md`
  - approach: a script that, in a throwaway container with darktable installed, develops each fixture with a fixed XMP sidecar (only the ported modules enabled, no tone mapping) to a 32-bit TIFF in the same working space; the TIFF reference is committed as a small fixture. The test runs the Develop chain on the same file and compares with a tolerance per method (PPG and RCD close, AMaZE and Markesteijn within a looser bound), reporting mean and max error.
  - verify: the test passes for each fixture; the README states the tolerances and why.
  - size: M

- [ ] M80.P5.T2 — Large-image and threading check
  - files: new `Tests/Native/RawLarge_Test.cpp`, `tools/bench/`
  - approach: a synthetic 100 MP mosaic (generated at run time, never committed) through the whole chain: no `int` overflow (`size_t` row arithmetic), memory bounded by the decode cache cap, and the chain scales across threads without lock convoy (check with the `tools/bench` scripts, per the render-scaling findings). Concurrent renders of two views of one file share one decode.
  - verify: gtest completes under a stated memory bound; bench shows time falling with thread count on the many-core box.
  - size: M

- [ ] M80.P5.T3 — Panels and viewer check under Xvfb
  - files: `Tests/gui/m80_raw.py`, node panel layouts in `Engine/Nodes/Raw/*.cpp` (labels and pages only)
  - approach: run the Xvfb recipe (`build/deeprepro/run-gui.sh`, `checkForUpdates` pre-seeded off): open a Bayer and an X-Trans fixture, Develop, switch demosaic methods, change temperature, and capture screenshots of each node's panel and the viewer. Share the screenshots and get the user's approval before sign-off (project rule).
  - verify: scripts pass under Xvfb; the screenshots are shared and the user approves them.
  - size: M

## Phase 80.6: GPU backend decision

- [ ] M80.P6.T1 — Raw-kernel requirements for the engine GPU backend
  - files: a `PLAN/DESIGN/` note (written by the PM from the spike), spike code under `build/` only
  - approach: the GPU backend is chosen once, engine-wide, in M83 - GPU Compute Backend Spike (Vulkan compute + Slang is the lean; OpenCL and GL compute are the alternatives it measures). This task does not pick one. It records what raw kernels need from that backend: the darktable kernels to port first (RCD demosaic, white balance, highlight reconstruction, denoise) and their OpenCL constructs (local memory, barriers, image sampling, float precision), the per-frame working-set size, and the CPU/GPU tolerance a reference comparison can hold. Port one kernel (RCD or white balance) to Slang's C++ target, or to plain C++, as a CPU twin, to size the porting effort. Keep the many-core/modest-machine range in mind: CPU stays the default, GPU optional.
  - verify: the note exists and M83 - GPU Compute Backend Spike's brief references it.
  - size: M

**Verification gate:** `ctest` green including every new `Tests/Native/Raw*` and `NativeReadRaw` test; the darktable reference comparison (M80.P5.T1) passes within its stated tolerances on a Bayer and an X-Trans fixture; the large-image test (M80.P5.T2) passes; the Xvfb script (M80.P5.T3) passes and the user has approved the screenshots; the raw-kernel requirements note (M80.P6.T1) is written.
