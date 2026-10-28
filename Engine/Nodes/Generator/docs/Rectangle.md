# Rectangle

A pure generator: it has no inputs, no mask and no mix. To use it over an image, merge the Rectangle over it.

| Knob (script name) | Default | Notes |
|---|---|---|
| Corner Radius (`cornerRadius`) | 0, 0 | The horizontal and vertical radii of the rounded corners, in canonical pixels. Each is clamped to half the rectangle's shorter side. |
| Softness (`softness`) | 0 | How far inside the edge, in canonical pixels, the rectangle fades from Color 1 to Color 0. At 0 the edge is hard but antialiased. |
| Falloff (`falloff`) | Quadratic | Linear, Smooth, Ease In, Ease Out or Quadratic: the curve the edge follows across its softness. |
| Color 0 (`color0`) | 0, 0, 0, 0 | The colour outside the rectangle. |
| Color 1 (`color1`) | 1, 1, 1, 1 | The colour inside the rectangle. |

The rectangle fills the generator's extent, which is set by the extent knobs every native generator has. Colours are scene-linear and pass through unclamped.
