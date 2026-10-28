# M79 - Layered Document Readers

Brings back reading of layered document formats dropped when Read became native-only: Photoshop (PSD, PSB, PDD), Krita (KRA), OpenRaster (ORA) and GIMP (XCF). Today these come from OFX readers (ReadPSD on ImageMagick, ReadKrita, OpenRaster), which the native Read replaced. Likely shape: per-format decoders feeding the native Read, with document layers mapped onto Natron layers per the layer registry. See `DECISIONS/2026-10-09-native-read-format-scope.md`.

Acceptance sketch:
- A PSD/PSB, KRA, ORA and XCF file each read through Read, with their layers available as Natron layers.
- No OFX reader is involved.

## Scope

**In:**
- In-tree readers registered as OIIO `ImageInput`s (pending Q1): one subimage per layer plus the composite; full window = canvas.
- Containers read with `Serialization/Bundle/ZipStore.h` and `QXmlStreamReader`; embedded PNGs with OIIO from memory.
- Layer model (pending Q2): stored composite = colour plane; raw layers become Natron layers, group paths flattened to sanitised names (pass `LayerRegistry::validate`; collisions get `_2`, `_3`…); blend mode, opacity, visibility, clipping and group path in metadata; layer masks folded into alpha.
- RGB and Gray only, at each format's stored depths (PSD 8/16/32, KRA 8/16/F16/F32, ORA PNG 8/16, XCF 8/16/32 int, half, float).

**Out:** re-compositing blend modes; adjustment/filter/fill/clone/text/vector layers (skipped, listed in metadata); CMYK, Lab, indexed, duotone (refused with an error naming the mode); writing these formats.

**Edge cases:**
- Empty layers and PSD group dividers (OIIO's psd reader crashes on these): skipped as pixels, kept as group structure.
- Layers outside the canvas: cropped.
- PSB up to 300 000 px: headers parse without pixels; decode per window via RLE row offsets / per-tile decode.
- XCF has no stored composite: see Q2. A PSD without "maximize compatibility" (empty composite) falls back to the XCF rule.
- OIIO's built-in psd plugin owns `.psd`: P1.T1 decides whether ours wins or Read opens by format name.

**Assumptions:** `OIIO::declare_imageio_format` exists in OIIO 3.1.16. Krita's LZF codec derives from liblzf (port is GPL-2+ compatible); XCF written from GIMP's `devel-docs/xcf.txt` only, not GPL-3 code.

## Phase 79.1: Reader seam, layer model and OpenRaster

- [ ] M79.P1.T1 — In-tree ImageInput seam
  - files: `Engine/Nodes/IO/LayeredDocs/LayeredDocument.{h,cpp}`, `LayeredImageInput.{h,cpp}` (new), `Engine/Nodes/IO/OiioReadSupport.cpp`, `Engine/CMakeLists.txt`, `Tests/Native/LayeredDocsSeam_Test.cpp`
  - approach: LayeredDocument (canvas, composite, layers with name path/bounds/mode/opacity/visible/clipping/depth, per-window fetch) exposed by LayeredImageInput as subimages; each format declared once with declare_imageio_format before readableExtensions(); remove `psd` from kExcludedOiioFormats (:820), add psb, pdd, kra, ora, xcf; prove with a test-only format.
  - verify: extensions listed; test format opens through readHeader; subimages match; `.psd` resolves to ours.
  - size: L
- [ ] M79.P1.T2 — Document layers as Natron layers and metadata
  - files: `OiioReadSupport.cpp` (fileLayers, attributeMetadata), `LayeredDocument.cpp`, `Tests/Native/NativeReadLayeredDocs_Test.cpp`
  - approach: sanitising/collisions per Scope; metadata `layered/<layer>/{blendMode,opacity,visible,clipping,group}`; composite = colour plane; Gray fills RGB (M75).
  - verify: Read lists the sanitised layers and renders each; metadata reaches a downstream Dot.
  - size: M
- [ ] M79.P1.T3 — OpenRaster reader
  - files: `Engine/Nodes/IO/LayeredDocs/OraInput.cpp`, `Tests/Native/NativeReadOra_Test.cpp`, `Tests/fixtures/layered/*.ora`
  - approach: stack.xml (nested stacks → groups, offsets, opacity, visibility, composite-op), layer PNGs, mergedimage.png; fixtures ≤32 px incl. a nested group and an empty layer.
  - verify: `ctest -R NativeReadOra`.
  - size: M

## Phase 79.2: Photoshop and Krita

- [ ] M79.P2.T1 — PSD/PSB/PDD reader
  - files: `Engine/Nodes/IO/LayeredDocs/PsdInput.cpp`, `Tests/Native/NativeReadPsd_Test.cpp`, `Tests/fixtures/layered/*.{psd,psb}`
  - approach: Adobe's spec: header, image resources, layer and mask info (lsct dividers, luni names, user masks into alpha; raw/RLE/ZIP/ZIP-predicted channels); PSB 8-byte lengths; merged image data = composite; PDD read as PSD.
  - verify: 8/16/32 bit, groups with empty layers, masks, a PSB, a CMYK file refused.
  - size: L
- [ ] M79.P2.T2 — Krita reader
  - files: `Engine/Nodes/IO/LayeredDocs/KraInput.cpp`, `Lzf.{h,cpp}`, `Tests/Native/NativeReadKra_Test.cpp`, `Tests/fixtures/layered/*.kra`
  - approach: maindoc.xml (paint, group, transparency masks), KRA tiles with LZF, RGBA/GrayA 8/16/F16/F32, mergedimage.png composite; other layer kinds skipped and listed.
  - verify: `ctest -R NativeReadKra` incl. an LZF round trip on a known vector.
  - size: L

## Phase 79.3: GIMP, GUI check and docs

- [ ] M79.P3.T1 — XCF reader
  - files: `Engine/Nodes/IO/LayeredDocs/XcfInput.cpp`, `Tests/Native/NativeReadXcf_Test.cpp`, `Tests/fixtures/layered/*.xcf`
  - approach: spec only; versions up to current incl. 64-bit offsets; every precision; RLE and zlib tiles; groups and masks; composite per Q2.
  - verify: 8-bit legacy, 32-bit float, groups, masks.
  - size: L
- [ ] M79.P3.T2 — Docs and licence notes
  - files: `Engine/Nodes/IO/docs/NativeRead.md`, third-party licence list (LZF)
  - approach: formats, layer model, metadata keys, what's refused.
  - verify: every key in the code documented.
  - size: M
- [ ] M79.P3.T3 — Xvfb GUI check
  - files: `Tests/gui/m79_layered_read.py`
  - approach: Read each fixture; layer menu lists document layers; view composite and one layer; screenshots.
  - verify: passes on the remote GUI runner.
  - size: M

**Verification gate:** remote `release --tests` green incl. `NativeRead*`/`LayeredDocs*` and smoke; GUI check passes; screenshots approved; no OFX reader loaded; AppImage for UAT.

## Open questions (for the user)
- Q1 Library choice (still undecided). Recommendation: in-tree OIIO ImageInputs on ZipStore and QXmlStreamReader (the scout named minizip-ng/pugixml, which Natron doesn't reference).
- Q2 Layer model, and XCF's composite. Recommendation: the scout's model; for XCF the reader flattens visible layers with Normal over and lists other blend modes in metadata.
- Q3 Fixtures. Recommendation: tiny files saved from GIMP and Krita (which also export PSD and ORA) plus hand-built edge cases, ≤32 px, a few KB.

## Decisions
- 2026-10-28: elaborated (consultant); blocked on Q1–Q3 (the 2026-10-10 decision requires the library recommendation to go to the user first).
- 2026-10-10 — **Library choice (user):** when M79 comes up, a consultant scouts decoding libraries per format and the PM puts the recommendation to the user before elaborating.
- 2026-10-21 — Library scout (consultant, not yet put to a decision): no existing library fits. OIIO 3.1.16's psd reader aborts on any empty layer, and every PSD group is stored as one. OIIO #5562 skips them, dropping the group structure, and exposes no layer attributes either way. ImageMagick's xcf reader is 8-bit only, has no zlib tiles and no groups. Recommended: in-tree readers registered as OIIO `ImageInput` plugins (one subimage per layer plus the composite; full window = canvas). PSD to the Adobe spec; ORA with minizip-ng + pugixml + OIIO PNG (all already linked); KRA the same plus Krita's LZF tile format (port from Krita, GPL-2+); XCF from the spec only (GIMP code is GPL-3). Proposed layer model: stored composite as the colour plane; raw layers with mode, opacity, visibility and clipping in metadata; groups flattened into sanitised names (`LayerRegistry::validate` refuses spaces and dots); masks folded into alpha; RGB and Gray only. Claimed and released the same day: the user paused new milestones before deciding.
