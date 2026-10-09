# Read is always native and covers every OIIO format

Decided 2026-10-09 with the user, after a runtime survey of the 105 extensions the OFX readers registered.

- The native Read (M75 - Native Read) reads every format OpenImageIO supports, taken from OIIO's own `extension_list` at runtime: EXR, PNG, JPEG, JPEG-2000/XL, TIFF, DPX/Cineon, HDR, PFM, PNM, Targa, SGI, BMP, WebP and the rest. PNG and PFM, which had dedicated OFX readers, move to it too.
- **RAW camera formats are dropped.** They will come back later through a different technique, not OIIO's libraw plugin, so the native Read excludes them even though OIIO lists them.
- **The OFX Read container path is removed.** Read is always the native node; there is no extension-based fallback. xpm and miff (ImageMagick) are dropped. Video stays unsupported until a native video reader exists (ReadFFmpeg was never in this fork's plugin bundle). Explicit OFX reader IDs stop being user-facing.
- **Layered documents come back in a later milestone, M79 - Layered Document Readers:** PSD/PSB/PDD, Krita (KRA), OpenRaster (ORA) and GIMP (XCF).

This supersedes the "video and RAW stay on the OFX readers through a fallback" clause of `2026-10-08-native-io-and-metadata-design.md`. The Write side (M76 - Native Write) is expected to mirror it, to be confirmed at M76's promotion.
