# Milestone 66: Remaining OFX plugins accept alpha-only streams

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Patch the 12 "moderate" plugins from M65's survey so they process alpha-only streams natively instead of through the host's widen-and-narrow fallback: the 9 Arena ImageMagick effects (Arc, Charcoal, Edges, Implode, Oilpaint, Polar, Reflection, Sketch, Tile; one shared RGBA lock in `Magick/MagickPlugin.cpp` `MagickPluginHelperBase`, plus a literal "RGBA" Magick I/O in each), openfx-io's ReadEXR (`decode()` requires 4 components), and openfx-misc's TimeBufferRead/Write (RGBA-shaped shared buffer). Also fix the duplicate plugin ID `net.sf.openfx.HueCorrect` (`HueCorrect.cpp` and `HueCorrect1.cpp`), and review Arena's two `net.fxarena.openfx.Text` versions.

Blocked on: M65 shipping its host fallback (P8.T6) and the fork workflow from P8.T7–T10, which this milestone reuses.

Acceptance sketch:
- An alpha-only stream through each of the 12 plugins stays alpha-only, with no host round-trip.
- Only one plugin registers `net.sf.openfx.HueCorrect`.

## Decisions

- 2026-10-02 — Duplicate plugin IDs (user): keep both implementations; the older one is re-registered under a distinct ID, rather than deleted.
