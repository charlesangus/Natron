# CornerPin

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| CornerPin | resampling | `kResamplingTolerance` | concatenation against one combined resample |
| CornerPin | iir | `kMotionBlurTolerance` | motion blur against Transform over the same shutter |

The matrix tests compare against hand-computed matrices to 1e-9 after normalising by the
bottom-right element.

Differences from the openfx-misc CornerPin and CornerPinMasked:

- One node: the Mask input and the maskInvert and mix knobs replace CornerPinMasked.
- `invert` inverts the whole transform, extra matrix included, so the forward matrix the host
  concatenates is the exact inverse of the sampling one. With no point enabled, the extra matrix
  is inverted too.
- Degenerate points fall back over the subsets of one point fewer, later points dropped first,
  down to one point; the z<0 sign flip uses the first point of the subset that solved.
- No directional blur.
- The matrix is offered to the host only while the mask is not applied and mix is 1 at the time
  asked.
