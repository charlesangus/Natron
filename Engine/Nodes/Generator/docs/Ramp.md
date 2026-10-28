# Ramp

A pure generator: it has no inputs, no mask and no mix. To ramp over an image, merge the Ramp over it.

| Knob (script name) | Default | Notes |
|---|---|---|
| Point 0 (`point0`) | 100, 100 | Where the ramp is Color 0, in canonical coordinates; driven by the viewer overlay. |
| Color 0 (`color0`) | 0, 0, 0, 0 | The colour at Point 0 and before it. |
| Point 1 (`point1`) | 100, 200 | Where the ramp is Color 1, in canonical coordinates; driven by the viewer overlay. |
| Color 1 (`color1`) | 1, 1, 1, 1 | The colour at Point 1 and beyond it. |
| Falloff (`falloff`) | Linear | Linear, Smooth, Ease In, Ease Out or Quadratic: the curve between the two colours. |

The ramp runs along the line from Point 0 to Point 1 and is constant across it. Colours are scene-linear and pass through unclamped. Coincident points give Color 1 everywhere. The ramp lives in canonical space, so every render scale samples the same gradient.

Its plugin ID is `Ramp`, major version 1; it does not share the ID of the OpenFX `net.sf.openfx.Ramp`.
