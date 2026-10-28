# VectorBlur

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| VectorBlur (VectorBlur) | Resampling | `tolerance()` in NativeVectorBlurGpu_Test.cpp (GPU) | GPU against CPU, 1e-5 absolute plus 1e-4 relative: both sides run the same float arithmetic, the GPU may fuse multiply-adds |
| VectorBlur (VectorBlur) | Exact | `ParityTolerance::make(1e-4, 1e-4)` (CPU) | A constant 8 pixel vector against the box integral of the bilinear Source, which 16 midpoint samples compute exactly |

Smears each pixel of the Source along its own motion vector, read from a layer of the Source.

- **Vectors.** UV picks the layer and its channels: u is the first selected channel, v the
  second, and a missing one reads 0. Auto, the default, takes a layer named `motion`, else the
  forward motion layer. Vectors are in pixels, or with Vector Units at normalised in fractions of
  the Source format's width (both u and v). Scale multiplies them; the render scale applies.
  A non-finite or zero vector leaves the pixel unchanged.
- **Streak.** The pixel centre `c` is averaged over `c + (u - Offset) * d` for `n` stratified
  `u = (i + 0.5) / n`. Offset 0.5 centres the streak on the pixel, 0 smears it back along the
  vector (the pixel gathers from ahead of it), 1 forward. Taps are bilinear, black outside the
  Source.
- **Samples.** `n = min(Max Samples, max(Samples, ceil(length)))`: at least Samples, about one per
  pixel of length, never more than Max Samples (128 by default, never below Samples).
- **Filter.** Box weighs every sample alike; Triangle weighs them by a tent peaking at the
  streak's middle.
- **Max Length.** Vectors longer than Max Length (128 pixels by default) are shortened to it. A
  render that shortens one posts a warning on the node; changing UV, Scale, Vector Units or Max
  Length takes it back. Only CPU renders post it.
- **Occlusion.** Off, or Depth-aware with Depth (default `depth.Z`) in Depth Units: a distance,
  or an inverse distance (1/z). A non-finite or non-positive depth is infinitely far. A sample
  whose nearest pixel is nearer than the centre by more than 1% is dropped, unless that pixel's
  own streak passes within one pixel of the centre: a static foreground stays sharp over a
  moving background, a moving foreground still smears over it. A pixel whose samples are all
  dropped is unchanged.
- **Identity.** Scale 0, or no UV layer that resolves, leaves the Source unchanged.
- **Region.** The Source, including the UV and depth layers, is read over the render window
  grown by `ceil(Max Length * render scale) + 2`: vectors are unknown before rendering, which is
  why Max Length bounds them. The region of definition is the Source's.
- **Knobs.** UV, Vector Units, Offset, Scale, Samples, Max Samples, Filter, Max Length,
  Occlusion, Depth, Depth Units, and the standard channel, mask and mix knobs. Mask and mix are
  the host's stages; as a filter, VectorBlur has no "(Un)premult by".

There is no OFX VectorBlur to port; the controls follow the milestone and Nuke's VectorBlur.
