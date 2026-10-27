# HueCorrect

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| HueCorrect | table | one table step | the curves are read from the same sampled tables on both sides; a hue a rounding away from a table entry boundary may read the neighbouring interval |

The node replaces OpenFX's HueCorrect (`net.sf.openfx.HueCorrect`, version 2) with every
control, under the same names and defaults, except where listed below
(`Tests/Native/NativeHueCorrect_Test.cpp`). The ten periodic curves keep their names, order,
ranges and default points.

- **Maths.** Each curve is read at the source's hue. In order: hue rotates by half of
  (hue curve - 1) turns, keeping the largest channel and the chroma; each `*_sup` curve `k`
  moves its channel above the smaller of the other two, `m`, to `m + k·(c - m)`; above the
  sat_thrsh saturation `t`, each channel is scaled by `(t + (s - t)·gain) / s`, with gain the
  channel's curve times lum (and by the gain itself when `t` is 0); the sat curve blends with
  the luminance, `l + sat·(c - l)`; Mix Luminance scales towards the input luminance by
  `1 + mix·(lIn / lOut - 1)`; then Clamp Black and Clamp White.
- **Scene-linear.** Saturation is the chroma over the largest of R, G and B, 0 when that is not
  positive, and nothing is clamped unless Clamp Black or Clamp White is on, so HDR values are
  scaled rather than clipped. A hue shift keeps the largest channel and the chroma, so a colour
  whose largest channel is not positive keeps its chroma.
- **Luminance Math affects the render.** OpenFX declared it with no re-render on change; here the
  sat curve and Mix Luminance read it, and changing it re-renders.
- **Curves are sampled into 1025-entry tables** over the whole 0 to 6 domain, in float. OpenFX
  used 1024 entries for float images.
- **Premult, mask and mix are the host's stages.** OpenFX's premultChanged is dropped.
- **No picker line** in the curve background. The hue ramp and the Hue Vs Hue Guide are drawn.
- **Identity** when every curve is at its default and no clamp or luminance mix is set.

The kernel reads the same tables and follows the CPU statement for statement
(`Tests/Native/NativeHueGpu_Test.cpp`).
