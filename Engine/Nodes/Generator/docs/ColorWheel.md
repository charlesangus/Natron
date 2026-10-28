# ColorWheel

A pure generator: it has no inputs, no mask and no mix. To use it over an image, merge the ColorWheel over it.

| Knob (script name) | Default | Notes |
|---|---|---|
| Center Saturation (`centerSaturation`) | 0 | The HSV saturation at the centre of the wheel, 0 to 1. |
| Edge Saturation (`edgeSaturation`) | 1 | The HSV saturation at the edge of the wheel, 0 to 1. |
| Center Value (`centerValue`) | 1 | The HSV value at the centre of the wheel. Above 1 is HDR. |
| Edge Value (`edgeValue`) | 1 | The HSV value at the edge of the wheel. Above 1 is HDR. |
| Rotate (`rotate`) | 0 | The rotation of the hues around the wheel, in degrees. Negative values rotate clockwise. |
| Softness (`softness`) | 0 | How far inside the wheel's edge, in canonical pixels, its alpha fades to 0. At 0 the edge is hard but antialiased. |

The wheel is inscribed in the generator's extent, which is set by the extent knobs every native generator has.
