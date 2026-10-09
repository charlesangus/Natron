# M79 - Layered Document Readers

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Brings back reading of layered document formats dropped when Read became native-only: Photoshop (PSD, PSB, PDD), Krita (KRA), OpenRaster (ORA) and GIMP (XCF). Today these come from OFX readers (ReadPSD on ImageMagick, ReadKrita, OpenRaster), which the native Read replaced. Likely shape: per-format decoders feeding the native Read, with document layers mapped onto Natron layers per the layer registry. See `DECISIONS/2026-10-09-native-read-format-scope.md`.

Blocked on: M75 - Native Read shipping (the native reader structure and layer mapping these extend) and a choice of decoding libraries for each format.

Acceptance sketch:
- A PSD/PSB, KRA, ORA and XCF file each read through Read, with their layers available as Natron layers.
- No OFX reader is involved.
