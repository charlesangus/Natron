# Reformat

## Filters

Transform and Reformat, and every node pointing here, resample through one list of filters. The
radius is the kernel's half-width in source pixels before any widening.

| Filter | Radius | When to use it |
|---|---|---|
| Impulse | 0.5 | Nearest source pixel, unfiltered. For drafts and for data that must not be blended, such as IDs. |
| Box | 0.5 | Averages the area under the pixel. Flat, for integer downscales of graphics. |
| Bilinear | 1 | Fast and slightly soft, for small moves and previews. |
| Cubic | 1 | The default: smooth, without overshoot. |
| Keys | 2 | Catmull-Rom. Slightly sharper than Cubic, with minor overshoot. |
| Simon | 2 | Medium sharpening, for repeated transforms that would otherwise soften. |
| Rifman | 2 | Significant sharpening, with visible overshoot on hard edges. |
| Mitchell | 2 | Slight blur that hides pixelation when upscaling. |
| Parzen | 2 | Cubic B-spline, the smoothest of the cubics. For soft material and noisy sources. |
| Notch | 2 | Flat smoothing that tends to hide moire patterns. |
| Lanczos 2 | 2 | Windowed sinc. Sharp, with little ringing, for general downscaling. |
| Lanczos 3 | 3 | Windowed sinc. Sharper, with moderate ringing, for high-quality downscaling. |
| Lanczos 4 | 4 | Windowed sinc. The sharpest Lanczos and the most ringing, for clean detailed plates. |
| Sinc | Lobes | Hann-windowed sinc whose width is set by Lobes. |
| Blackman Harris | 2 | Soft, with very little aliasing, for fine repeating detail. |
| Gaussian | 1.5 | Soft, without overshoot, for mattes and other material that must not ring. |
| Bicubic | 2 | Mitchell-Netravali, shaped by Bicubic B and Bicubic C. |

**Lobes**, from 2 to 8, applies to Sinc only and is both its lobe count and its radius: more lobes are sharper, ring
more and read more of the source. **Bicubic B** and **Bicubic C** apply to Bicubic only. B blurs and C
sharpens: (0, 0.5) gives Keys and (1/3, 1/3) gives Mitchell. Each control is enabled only while its
filter is selected, and **Clamp** only for a filter that can overshoot.

The kernel widens when minifying, per source axis, by the length of that axis's row of the sample
point's Jacobian, so a 4x downscale under Lanczos 3 has a radius of 12 source pixels. It never narrows
when magnifying. Impulse does not widen. A sample outside the source uses the plain radius, because
there is nothing there to average. A footprint needing more than 81 taps per axis reads 81, spread
evenly over the whole footprint with bilinear reads between source pixels, so its cost stops growing
and very large minifications stay filtered.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Reformat | resampling | `kReformatTolerance` | |

GPU tolerance: `resampling()` (1e-5 absolute) for every resize type, centre, flip, flop and turn, every filter with clamp and black outside on and off, preserveBB on and off, at mipmap 0 and 1. The matrix is the CPU's, built on the host. A transform concatenated from upstream stays on the CPU (`Tests/Native/NativeReformatGpu_Test.cpp`).
