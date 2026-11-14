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

`showLowConfidence` reads this layer on the viewer: while the node's panel is open, pixels below
`confidenceThreshold` are tinted red where `Forward` is low and blue where `Backward` is low, over
whichever layer the viewer shows. On the first and last frames the side with no neighbour tints the
whole picture, since its confidence is a true 0: the first frame is all blue for `Backward`, the last
all red for `Forward`. A direction left out by `outputs` draws nothing. The tint is
drawn only on the viewer and changes no output, and it does not show during playback.

Moving `confidenceThreshold` redraws from the cached confidence without solving again. If a frame's
confidence is not in the cache, the tint solves it first, on the render thread, and asks for every
layer the node produces, `Forward`, `Backward` and `vectorConfidence`, in that one solve. The viewer
showing `Forward` or `Backward` then reads them from the cache, so a frame is never solved twice for
the tint. Showing RGBA, which otherwise costs no solve, waits for that solve; the viewer stays
responsive and the render can be cancelled.

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

## Neural method

`method` Neural runs an optical-flow neural network instead of a classical solve. It follows large and
thin motion the other methods lose. The model is the one installed for the `optical-flow` task, picked
by `model` and downloaded with `downloadModel` (or in Preferences > AI Models); both are shown only for
Neural. It runs on the CPU, so the node ends a GPU region, and each frame pair costs two model runs
(forward and backward), and temporal smoothing multiplies the pairs. Large frames are run downscaled to fit the model and the vectors scaled back to full size.

`modelColourspace` is the OpenColorIO colourspace the Source is converted to before the model, as on the
other neural nodes; it defaults to the one the model was trained in (sRGB) and is shown only for Neural.
The frames go to the model as they are, without the matching curve below.

`quality` sets the model's refinement iterations: 2 at Fast, 4 at Medium (what the model was trained
with) and 8 at Fine. Most of a run is the model's feature extraction, which every quality pays, so
Fast saves less time than it does for DIS or TV-L1 and is rougher on fine motion. `flowChannels` does
not apply to Neural and is greyed out: the model always reads the three colour channels. The
confidence layer, Matte, `temporalSmoothing` and `outputs` work as for the other methods. With a Matte, the foreground and
background runs show the model the frame multiplied by the matte and by its inverse, rather than
weighting a data term.

A missing model, or AI turned off in Preferences, is an error on the node saying why and pointing to the
model manager. The node never falls back to another method, and a project keeps `method` Neural.
Aborting a render stops the model run.

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
| `method` | DIS | The estimator: DIS, TV-L1 or Neural. |
| `quality` | Medium | The method's preset: Fast, Medium or Fine. For Neural, 2, 4 or 8 refinement iterations. |
| `flowChannels` | Luminance | What is matched: Luminance or RGB. Not used by Neural. |
| `model` | optical-flow default | The optical-flow model; Neural only. |
| `modelColourspace` | the model's | The colourspace the Source is converted to before the model; Neural only. |
| `matteChannel` | RGBA.A | The channel of the Matte input that separates foreground from background. |
| `temporalSmoothing` | off | Blends vectors with those of the frames around, by confidence. |
| `smoothingStrength` | 0.5 | How much those frames count; only with `temporalSmoothing`. |
| `showLowConfidence` | off | Tints the viewer where `vectorConfidence` is below `confidenceThreshold`, while the panel is open. It changes no output. |
| `confidenceThreshold` | 0.5 | The confidence below which `showLowConfidence` tints a pixel. It changes no output. |
| `outputs` | Forward+Backward | Which motion layers are produced; a layer left out passes through from the Source if it has one. |

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| VectorGenerator (VectorGenerator) | iterative solve | `ParityTolerance::make(1e-3, 0.)` | Regression against recorded references; a small rounding difference grows over a solve's iterations. |
