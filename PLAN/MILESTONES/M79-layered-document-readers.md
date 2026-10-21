# M79 - Layered Document Readers

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Brings back reading of layered document formats dropped when Read became native-only: Photoshop (PSD, PSB, PDD), Krita (KRA), OpenRaster (ORA) and GIMP (XCF). Today these come from OFX readers (ReadPSD on ImageMagick, ReadKrita, OpenRaster), which the native Read replaced. Likely shape: per-format decoders feeding the native Read, with document layers mapped onto Natron layers per the layer registry. See `DECISIONS/2026-10-09-native-read-format-scope.md`.

Blocked on: M75 - Native Read shipping (the native reader structure and layer mapping these extend) and a choice of decoding libraries for each format.

Acceptance sketch:
- A PSD/PSB, KRA, ORA and XCF file each read through Read, with their layers available as Natron layers.
- No OFX reader is involved.

## Decisions
- 2026-10-10 — **Library choice (user):** when M79 comes up, a consultant scouts decoding libraries per format and the PM puts the recommendation to the user before elaborating.
- 2026-10-21 — Library scout (consultant, not yet put to a decision): no existing library fits. OIIO 3.1.16's psd reader aborts on any empty layer, and every PSD group is stored as one. OIIO #5562 skips them, dropping the group structure, and exposes no layer attributes either way. ImageMagick's xcf reader is 8-bit only, has no zlib tiles and no groups. Recommended: in-tree readers registered as OIIO `ImageInput` plugins (one subimage per layer plus the composite; full window = canvas). PSD to the Adobe spec; ORA with minizip-ng + pugixml + OIIO PNG (all already linked); KRA the same plus Krita's LZF tile format (port from Krita, GPL-2+); XCF from the spec only (GIMP code is GPL-3). Proposed layer model: stored composite as the colour plane; raw layers with mode, opacity, visibility and clipping in metadata; groups flattened into sanitised names (`LayerRegistry::validate` refuses spaces and dots); masks folded into alpha; RGB and Gray only. Claimed and released the same day: the user paused new milestones before deciding.
