# M296 - Raw Prepare Output Format

Bug report from the user (2026-11-01): after RawPrepare, black still shows around the edge of the image. A scout (2026-11-01, on `origin/main`) found the cause. RawPrepare crops the data and puts its region of definition at (0,0) (`Engine/Nodes/Raw/RawPrepare.cpp` `getRegionOfDefinition` ~:517). It has no `getPreferredMetadataAt` override, though. Its output format therefore falls through to `EffectInstance::getDefaultMetadataFromInputs` (`Engine/EffectInstance.cpp` ~:6887), which copies the Read's full-sensor format (`Engine/Nodes/IO/RawDecodeSupport.cpp` ~:614, set at `NativeRead.cpp` ~:1085). The result is the cropped image at the bottom-left of a sensor-sized frame, with black strips on the right and top. The CPU and GPU crops agree, and the y-flip is correct. Every downstream raw node inherits the input's format, so fixing RawPrepare fixes the whole chain.

The milestone also checks whether rawspeed's crop window (the Read's RoD) honours the DNG DefaultCrop, or only the ActiveArea. If it is only the ActiveArea, DNGs keep a thin border of real image pixels (user, 2026-11-01).

Deps: M80 - Native RAW Support.

## Scope

**In:**
- RawPrepare sets its output format to the kept crop, `RectI(0, 0, kept.width(), kept.height())`, in pixels and not scaled by the pixel aspect ratio. This covers both the metadata crop and the override-margin crop.
- The crop knobs are metadata slaves, so editing them refreshes the format.
- Downstream raw nodes and the Viewer show the prepared format.
- A check that the Read window matches the DNG DefaultCrop, and a fix if it doesn't.

**Out:**
- The Read's own format stays the full sensor, so its black margins remain. That is by design: Natron formats start at (0,0), and the Read shows the whole sensor.
- Any other crop semantics (keeping an offset format with x1/y1 ≠ 0, a reformat choice on RawPrepare).

**Edge cases:**
- **Pass-through / Black route:** with no resolvable settings, or with RawPrepare disabled, the input format passes through unchanged.
- **An odd crop on a Bayer mosaic:** the format is the kept size exactly, the same as the RoD, with no CFA-alignment rounding.
- **Proxy / render scale:** a format is full-resolution, so it doesn't change with scale.

**Assumptions:**
- Unverified: calling `resolveSettings` (which reads the input RoD) from `getPreferredMetadataAt` doesn't re-enter metadata computation. `deriveOutputMetadata` already does the same at this stage.

## Phase 296.1: Prepared format

- [x] M296.P1.T1 — RawPrepare sets its output format to the kept crop
  - files: `Engine/Nodes/Raw/RawPrepare.h`, `Engine/Nodes/Raw/RawPrepare.cpp`
  - approach: Add a `getPreferredMetadataAt` override. It first calls `StagedImageEffect::getPreferredMetadataAt`, which handles the Black route. Then, when `resolveSettings` succeeds and the node is not passing through, it calls `metadata.setOutputFormat(RectI(0, 0, kept.width(), kept.height()))`. Follow the native Crop with Reformat (`Engine/Nodes/Transform/Crop.cpp` ~:549, where the crop becomes the format). Mark `_cropOverride` and the four crop-margin knobs `setIsMetadataSlave(true)`, as `Crop.cpp` ~:283 does. Confirm that `resolveSettings` doesn't re-enter metadata computation when called from this override. If it does, compute the kept rectangle from the input's metadata crop and the knobs directly.
  - verify: In `Tests/Native/NativeRawPrepare_Test.cpp`, extend `AnOddCropKeepsTheColourOfEveryPhotosite`, or add a sibling case on the synthetic `RawTestSource.h`. Assert that `getTimeInvariantMetadata()->getOutputFormat()` equals `RectI(0,0,crop.width(),crop.height())`, once for the metadata crop and once for the override-margin crop (the assertion style is in `NativeReadTime_Test.cpp` ~:672). Add a case that changes the override margins and checks that the format follows. Add a case where a RawDemosaic after RawPrepare reports the same format.
  - size: M

## Phase 296.2: DNG default crop

- [x] M296.P2.T1 — Check the Read window against the DNG DefaultCrop
  - files: `Engine/Nodes/IO/RawDecodeSupport.cpp`, `Engine/Nodes/IO/RawspeedRuntime.cpp`, `Tests/Native/RawDecodeSupport_Test.cpp`
  - approach: For the existing small DNG fixtures in `Tests/fixtures/raw/reference/`, find out whether rawspeed's crop (the header window at `RawDecodeSupport.cpp` ~:651-680, `setCrop` at ~:775) applies both ActiveArea and DefaultCropOrigin/DefaultCropSize, or only ActiveArea. Read the tags with rawspeed's TIFF IFD API, which is already linked. If only ActiveArea is applied, intersect the window with the DefaultCrop, mapped into the ActiveArea frame and then into Natron's y-up frame, in `RawDecodeSupport.cpp`. Record the finding in this file's `## Decisions` either way. Don't add any new camera raws (LFS quota).
  - verify: A `Tests/Native/RawDecodeSupport_Test.cpp` case asserts that, for a DNG fixture, the Read's format is the sensor size and its RoD has the DefaultCrop size (read from the file's tags inside the test). If the fixtures carry no DefaultCrop smaller than their ActiveArea, the case asserts that and the decision says so.
  - size: M

- [x] M296.P2.T2 — End-to-end format on a real raw
  - files: `Tests/Native/NativeReadRaw_Test.cpp`
  - approach: With `RawFixtures.h`, using the existing `Tests/fixtures/raw/canon-eos-5d-mark-iii.cr2` and one reference DNG, build Read → RawPrepare. Assert that the Read's format equals the sensor, its RoD equals the window, and RawPrepare's format equals the window's size at (0,0), with the RoD equal to the format.
  - verify: The new cases pass in `ctest`.
  - size: M

**Verification gate:** the full build and `ctest` pass. Under Xvfb (in the container or through the remote GUI checks), a GUI script loads the CR2 fixture through Read → RawPrepare → RawDemosaic and views it. It checks that the Viewer's format matches the prepared crop and that no black border remains at any edge. The screenshots go to the user before sign-off, and a release AppImage is packaged for the user's own test.

## Decisions

- M296 approved by the user 2026-11-06; merged as 2e50bb08b (#199).
- 2026-11-06 — Freshness check: P2.T1 and P2.T2 retargeted to `RawDecodeSupport_Test.cpp` and `NativeReadRaw_Test.cpp`, since `RawDevelop_Test.cpp` tests the chain builder.
- 2026-11-06 — rawspeed already applies both crops: `DngDecoder::handleMetadata` applies ActiveArea (`DngDecoder.cpp:537-539`), then DefaultCropOrigin and DefaultCropSize from its top-left (`:541-576`), silently ignoring out-of-range values. No code fix needed. The fixtures can't tell the two apart: the linear DNG's DefaultCrop equals its ActiveArea and the small reference DNGs carry neither.
- 2026-11-06 — The Unverified assumption is closed: `resolveSettings` reads only the input's RoD and metadata, never this node's own metadata, so it doesn't re-enter metadata computation.
- 2026-11-06 — Review (PR #199) closed: a closed-panel Viewer now refreshes on an upstream format change. Run 42175840639: GUI 31 of 31, legacy leg clean, taskgraph leg's only failure a `NativeSplineWarpGpuTest` timeout unrelated to this diff. UAT AppImage round 2 is at bac24f075.
