# M76 - Native Write

Replaces the `Write` container node with a native OIIO writer for EXR, PNG, JPEG and TIFF at minimum, writing the graph's metadata and handling very large images. The old OFX `WriteOIIO` overflows on large frames (`(height - 1) * rowBytes` with `int` operands at `WriteOIIO.cpp:1455` in the openfx-io source), so this milestone is the fix: no patch to the OFX plugin. Video (FFmpeg) stays on the OFX writer through a fallback. See `DECISIONS/2026-10-08-native-io-and-metadata-design.md`.

## Phase 76.1: Writer core

- [ ] M76.P1.T1 — Scout the Write contract and write the node skeleton
  - files: new `Engine/Nodes/IO/NativeWrite.h/.cpp`, `Engine/AppManager.cpp` (one registration line)
  - approach: study `Engine/WriteNode.cpp` and OFX GenericWriter behaviour (output file pattern, frame range, "render" button, overwrite policy, premult, colourspace through project OCIO, `isWriter` in the plugin description so the render scheduler treats it as an output). Check `WriteNode.cpp:762`, which calls the reader lookup where the writer lookup looks intended, and report it. Skeleton renders nothing.
  - verify: registers, shows in the Tab menu, saves/loads in a project, "Render" on the skeleton completes without crashing.
  - size: M

- [ ] M76.P1.T2 — OIIO encode with 64-bit addressing
  - files: `Engine/Nodes/IO/NativeWrite.cpp`, new `Engine/Nodes/IO/OiioWriteSupport.h/.cpp`
  - approach: `ImageOutput` create/open with the chosen format, per-format options (EXR compression and data type, PNG bit depth, JPEG quality, TIFF compression and bit depth). Write by scanline or tile chunks so peak memory is a band, not a second full-frame copy. Every size, stride and offset is `size_t`/`qint64`/`stride_t`; no `int` row arithmetic.
  - verify: gtest round-trips small EXR, PNG 8/16, JPEG, TIFF fixtures through write then OIIO read.
  - size: L

- [ ] M76.P1.T3 — Large-image gate test
  - files: new `Tests/NativeWriteLarge_Test.cpp`
  - approach: write a frame of at least 20000 x 12000 float RGBA (≈3.8 GB), over 2^31 bytes, using a generator and a native Write, to a temp file under `build/`. Read back the first, middle and last scanlines and compare. Skip with a clear log line when free disk or RAM is below the need (use a runtime check, not `GTEST_SKIP`, which this repo's gtest version lacks).
  - verify: passes in the container; fails when the stride type is deliberately narrowed to `int` (check once, then revert).
  - size: M

- [ ] M76.P1.T4 — Frame range, file pattern and render integration
  - files: `Engine/Nodes/IO/NativeWrite.cpp`
  - approach: `####`/`%04d` output patterns, frame range modes, create-directory option, overwrite policy, render-from-GUI and `-w` command line paths via the existing output scheduler. Confirm the M63 task-graph scheduler treats the node as a writer.
  - verify: gtest renders a 3-frame range to disk; a headless `NatronRenderer -w` run produces the same files.
  - size: M

## Phase 76.2: Metadata and fallback

- [ ] M76.P2.T1 — Write metadata out
  - files: `Engine/Nodes/IO/NativeWrite.cpp`, `Engine/Nodes/IO/OiioWriteSupport.cpp`
  - approach: map `ImageMetadata` keys back to `ImageSpec` attributes (`exr/*`, `exif/*`, timecode, framerate). Keys with no representation in the target format are dropped with a debug log. A knob lets the user choose "write metadata: all / none / only listed keys". Natron-owned `ofx/*` keys that describe the source file (`ofx/filepath`, `ofx/mtime`, `ofx/filesize`) are not written.
  - verify: gtest: Read an EXR with custom attributes → Write EXR → re-read; attributes survive. JPEG keeps EXIF.
  - size: M

- [ ] M76.P2.T2 — Fallback to the OFX writers for unsupported formats
  - files: `Engine/AppManager.cpp`, `Engine/WriteNode.cpp`
  - approach: mirror M75 - Native Read's fallback: the native Write wins for its four formats; other extensions build the old container with the OFX writer. Fix the `WriteNode.cpp:762` lookup if T1 confirmed it is wrong.
  - verify: script writes `.exr` (native) and `.mov` (OFX container); both outputs open.
  - size: M

**Verification gate:** large-image gate test passes; round-trip tests for all four formats, with metadata, pass; headless render works; fallback writes a `.mov`; full ctest and CI green. User signs off after a GUI render in an AppImage (see the AppImage testing memory).
