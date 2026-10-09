# M75 - Native Read

Replaces the `Read` container node with a native node on OpenImageIO, for EXR, PNG, JPEG and TIFF at minimum, emitting file metadata into the graph. Takes over the OFX plugin IDs at OFX major + 1 as M67 did (`AppManager::getPluginBinary`). Video (FFmpeg) and RAW formats stay on the OFX readers through a fallback. See `DECISIONS/2026-10-08-native-io-and-metadata-design.md`.

## Phase 75.1: Reader core

- [ ] M75.P1.T1 — Scout the Read contract and write the node skeleton
  - files: new `Engine/Nodes/IO/ReadNode.h/.cpp` (name must not collide with the existing `Engine/ReadNode.cpp`; pick `NativeRead`), `Engine/AppManager.cpp` (one `registerBuiltInPlugin<>` line)
  - approach: study `Engine/ReadNode.cpp`/`.h` and the OFX GenericReader behaviour it embeds (frame range, before/after policy, time offset, missing-frame handling, proxy, premult, colourspace through project OCIO per `DESIGN/2026-10-02-project-ocio.md`). Write the knob list in the node's header comment, then implement `getNativePluginDescription`, `initializeKnobs` and a stub `render` that reports a black frame. Model on `Engine/Nodes/Generator` and `DeepRead.cpp`.
  - verify: the node registers, appears in the Tab menu, and a project containing it saves, loads and renders black without crashing.
  - size: M

- [ ] M75.P1.T2 — OIIO image decode into native image buffers
  - files: `Engine/Nodes/IO/NativeRead.cpp`, new `Engine/Nodes/IO/OiioReadSupport.h/.cpp`
  - approach: `ImageInput` open (cached per file handle, thread-safe), read scanlines or tiles into the node's output image in the requested bit depth and channels. All sizes and strides in `size_t`/`qint64`. Honour the render ROI; read only the needed scanline range. Multi-part and multi-channel EXR planes become layers, per the layer registry decision (`DESIGN/2026-09-19-layer-registry.md`).
  - verify: gtest decodes small EXR (half and float, with an extra layer), PNG 8 and 16 bit, JPEG and TIFF fixtures and compares pixel values to OIIO's own read.
  - size: L

- [ ] M75.P1.T3 — Frame sequences, frame range and error policy
  - files: `Engine/Nodes/IO/NativeRead.cpp`, `Engine/Nodes/IO/OiioReadSupport.cpp`
  - approach: `####` and `%04d` patterns through the existing file-sequence helpers (find them in `Engine/` before writing new ones), first/last frame, time offset, hold/loop/bounce/error before and after, "missing frame: error / black / nearest" policy.
  - verify: gtest over a three-frame sequence covering each policy and the offset.
  - size: M

## Phase 75.2: Metadata and integration

- [ ] M75.P2.T1 — Emit file metadata
  - files: `Engine/Nodes/IO/NativeRead.cpp`, `Engine/Nodes/Metadata/ImageMetadata.h` (only if a missing helper is needed)
  - approach: override `getOutputMetadata` from M74 - Native Metadata Core. Convert the OIIO `ImageSpec` attributes (`exr/*`, `exif/*`, timecode, framerate, colourspace tags, `oiio:*` dropped or kept under `oiio/`) to prefixed keys, plus `ofx/filepath`, `ofx/frame`, `ofx/filesize`, `ofx/mtime`, `ofx/pixelaspect`.
  - verify: gtest reads an EXR fixture with custom attributes and checks keys and values; JPEG EXIF fields appear under `exif/`.
  - size: M

- [ ] M75.P2.T2 — Fallback to the OFX readers for unsupported formats
  - files: `Engine/AppManager.cpp`, `Engine/Nodes/IO/NativeRead.cpp`, `Engine/ReadNode.cpp` (existing)
  - approach: `getReaderPluginIDForFileType` keeps the OFX scoring but the native Read wins for its four formats. For any other extension the node creation path still builds the old container with the OFX reader, so FFmpeg and RAW files keep working. No project-compat shims (`memory: no backward compat`): old projects with the old Read need no loader.
  - verify: gtest/script that creates a Read for `.exr` (native) and for `.mov` (OFX container) and renders each.
  - size: M

- [ ] M75.P2.T3 — Layer/channel knobs and viewer integration
  - files: `Engine/Nodes/IO/NativeRead.cpp`, `Gui/` only if the existing layer widget needs a hook
  - approach: surface the file's layers in the existing layer-channel widget (`DESIGN/2026-09-19-layer-channel-widget.md`) and the output-layer selector. Colourspace knob uses project OCIO.
  - verify: Xvfb GUI check with a multilayer EXR: layers listed, selection changes the viewer. Share a screenshot before sign-off.
  - size: M

**Verification gate:** all four formats decode bit-exactly against OIIO in ctest; sequences and policies tested; OFX fallback renders a `.mov`; Xvfb GUI check with a multilayer EXR passes with a screenshot approved by the user; CI green.
