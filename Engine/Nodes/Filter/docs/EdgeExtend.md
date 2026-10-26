# EdgeExtend

Extends the colour of a matte's solid pixels outwards, over the pixels the matte leaves clear or
partly clear. The output alpha is always the input's.

1. The straight colour is the input's colour, divided by its alpha (where that exceeds 1e-6) with
   Source Premultiplied on (the default, as keyers output premultiplied images), or as it is with
   it off, for a straight input.
2. A pixel whose alpha is at least Threshold (default 0.5, the matte's edge) is solid. The running
   result starts as the straight colour with an alpha of 1 on the solid pixels, and as 0, colour
   and alpha, elsewhere.
3. Each of the `slices` slices (default 5) blurs the running result and merges the result over
   its blur, `a + b * (1 - alpha a)`, on all four channels. Slice `i` of `n` blurs at `i` times
   `size / sqrt(n (n + 1) (n + 0.5) / 3)`, so the slices' blurs add up to one of `size` (default
   10).
4. The extended colour is the result's colour divided by its grown alpha (where that exceeds
   1e-6): the straight colour on the solid pixels, the solid colour nearby elsewhere, and black
   past the slices' reach. The output colour is the extended colour, straight; with Premultiply
   Output on (off by default) it is multiplied by the input's alpha, a premultiplied image whose
   soft edges carry the extended colour.

The colour of the pixels below the threshold is discarded, so a contaminated soft edge takes the
solid colour nearby; Threshold 1 keeps only the opaque core's colour. Source Premultiplied on with
Premultiply Output on gives back a premultiplied input's opaque pixels unchanged and its
semi-transparent solid pixels within a rounding; off with off gives back a straight input's solid
pixels bit for bit.

The blurs are FIR Gaussians (Blur's "Gaussian (FIR)", standard deviation size / 2.4) that repeat
the nearest pixel outside the image, so the region of definition is the source's and nothing
spreads past the sum of the slices' radii. The node works on the four channels at once: with
fewer processed it passes the input through, as the OFX CImg EdgeExtend does. Size 0 passes the
input through unchanged. Mask and mix are the host's stages; the node does its own
unpremultiplication and premultiplication. Nothing is clamped.

EdgeExtend replaces the OFX CImg EdgeExtend, whose default IIR filter it does not offer: that
filter's tails spread colour indefinitely, and its documentation recommends FIR. Its ID,
`eu.cimg.EdgeExtend`, is retired (`Engine/OfxRetired/`): a project naming it gets the missing-plug-in
node.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| EdgeExtend (EdgeExtend) | resampling | `kTolerance` | GPU against CPU, FIR Gaussian slices |

The CPU properties (solid pixels and alpha unchanged, the edge colour outside the box, the
kernels' reach, the premultiplied and straight round trips, the threshold, size 0 is the
identity) are in
`Tests/Native/NativeEdgeExtend_Test.cpp`; the GPU parity is in
`Tests/Native/NativeEdgeExtendGpu_Test.cpp`.
