# VectorGenerator

Estimates the Source's motion between adjacent frames and writes it to the `Forward` and `Backward`
motion layers, with the agreement of the two directions in a `vectorConfidence` layer. RGBA and every
other layer pass through unchanged, so a node downstream sees the same picture with vectors beside it.

## Vector convention

Vectors are in pixels at the render scale: at half scale they are half as long. A vector at a pixel
points to where that pixel's content lies in the other frame. `Forward` goes from frame t to t+1 and
`Backward` from frame t to t-1; for a texture moving 3 pixels right a frame, `Forward` holds (3, 0) and
`Backward` (-3, 0). Each layer has the channels U and V, horizontal and vertical.

## Confidence layer

`vectorConfidence` has two channels, `Forward` and `Backward`, one per direction, each 0 to 1. It says
how closely a direction's vector and the opposite solve between the same two frames agree: a pixel that
returns to where it started has confidence near 1, and the confidence falls to 1/e at a round-trip
error of 2 pixels. Occluded areas read low.

The first and last frames of the Source have no neighbour on one side. That direction has zero vectors
and confidence 0, and the other direction is estimated as usual. A direction left out by `outputs` also
has confidence 0. The layer is always produced.

## Methods and quality

DIS (dense inverse search with variational refinement) is the default: fast, and good on most plates.
TV-L1 is a global variational solve: slower, with sharper motion edges and less drift on flat areas.
`quality` sets how many pyramid levels and iterations either method spends: Fast for a preview, Medium
for general use, Fine for the final render.

`flowChannels` chooses what is matched. Luminance is the cheaper; RGB also follows edges that differ
only in hue.

Matching works on a copy of the Source compressed by a log-like curve, so that scene-linear highlights
do not swamp the gradients in the shadows. Output vectors and passes are not affected by it. Values at
or below 0, and non-finite values, match as 0; the curve then rises steeply from black, with 1 mapping
to 1, and values above 1 keep rising more slowly. The curve has no control.

## Matte

An optional Matte separates a foreground from its background: the two are estimated apart, each with
the other weighted down, and composited by `matteChannel`, which keeps a foreground's motion from
bleeding into what lies behind it. The channel defaults to the Matte's alpha. A matte that is empty or
solid separates nothing and gives the result of no matte. A matte doubles the solves of the pairs of
frames it is part of.

## Temporal smoothing

`temporalSmoothing` blends each frame's vectors with those of the frames around it, of the same
direction and weighted by confidence, to steady them over time. `smoothingStrength` is how much those
frames count: 0 keeps the frame's own vectors, and at 1 three confident frames count equally. It reads
t-2 to t+2 instead of t-1 to t+1, and solves up to twice as many times.

## Cost

Every render estimates over the Source's whole region of definition, at each frame it reads, however
small the render window, and the result of a render covers the planes requested together. Each
direction costs two solves, one each way, for the confidence, so a frame costs up to four, or eight
with smoothing, and twice that with a matte. A later request for another layer of the same frame that
missed the cache solves again.

All frames are read over the current frame's region of definition, so vectors measure between the same
pixel positions when a Source's region animates; a neighbour is black where it has no pixels, even
when its whole region lies outside the current frame's. A neighbour whose format rectangle or pixel
aspect ratio differs from the current frame's fails the render, with an error naming both frames, as
does one with other channels when `flowChannels` is RGB: a frame with fewer than three channels is
matched on its first alone, which RGB frames cannot be compared with.

| Parameter | Default | Meaning |
|---|---|---|
| `method` | DIS | The estimator: DIS or TV-L1. |
| `quality` | Medium | The method's preset: Fast, Medium or Fine. |
| `flowChannels` | Luminance | What is matched: Luminance or RGB. |
| `matteChannel` | RGBA.A | The channel of the Matte input that separates foreground from background. |
| `temporalSmoothing` | off | Blends vectors with those of the frames around, by confidence. |
| `smoothingStrength` | 0.5 | How much those frames count; only with `temporalSmoothing`. |
| `confidenceThreshold` | 0.5 | A reference level for reading the `vectorConfidence` layer, kept with the node. Nothing draws it yet; it changes no output. |
| `outputs` | Forward+Backward | Which motion layers are produced; a layer left out passes through from the Source if it has one. |

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| VectorGenerator (VectorGenerator) | iterative solve | `ParityTolerance::make(1e-3, 0.)` | Regression against recorded references; a small rounding difference grows over a solve's iterations. |
