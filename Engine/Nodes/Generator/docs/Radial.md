# Radial

A pure generator: it has no inputs, no mask and no mix. To use it over an image, merge the Radial over it.

| Knob (script name) | Default | Notes |
|---|---|---|
| Softness (`softness`) | 0 | How far inside the edge, in canonical pixels, the ellipse fades from Color 1 to Color 0. At 0 the edge is hard but antialiased. |
| Falloff (`falloff`) | Quadratic | Linear, Smooth, Ease In, Ease Out or Quadratic: the curve the edge follows across its softness. |
| Color 0 (`color0`) | 0, 0, 0, 0 | The colour outside the ellipse. |
| Color 1 (`color1`) | 1, 1, 1, 1 | The colour inside the ellipse. |

The ellipse is inscribed in the generator's extent, which is set by the extent knobs every native generator has. Colours are scene-linear and pass through unclamped.
