# Duplicate OFX plugin IDs get distinct IDs; ImageMagick effects on alpha-only streams

2026-10-02, M66 (user decision on duplicates).

- **Duplicate IDs.** When two bundled plugins register the same ID (in different major versions), the older one is kept and re-registered under a distinct ID. It is never deleted. HueCorrect 1.0 is `net.sf.openfx.HueCorrect1`, and the ImageMagick Text 5.7 is `net.fxarena.openfx.MagickText`. The newer versions keep `net.sf.openfx.HueCorrect` and `net.fxarena.openfx.Text`. OFX allows one ID across major versions, so this is a policy for clarity, not a collision fix. No project compatibility shim (clean break).
- **ImageMagick effects on alpha-only streams.** The geometric effects (Arc, Implode, Polar, Reflection, Tile) treat the single channel as coverage, and their result equals the alpha that the RGBA path produces. The content effects (Charcoal, Sketch, Oilpaint, Edges) treat the matte as a grayscale picture and output a stylised single channel.
- **Native alpha in OFX plugins.** A plugin accepts an alpha-only stream natively when it declares `ePixelComponentAlpha` on its clips and renders one component. The host's widen-and-narrow fallback stays only for plugins that don't.
