# Bilateral

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Bilateral | transcendental | `kBilateralTolerance` = `ParityTolerance::make(0., 1e-4)` | GPU against CPU, 1e-4 relative: the weights go through `exp`, and the range key through `log2` |

Smooths the image while keeping its edges: each pixel is averaged with its neighbours, weighted by
their distance and by how far their luminance is from its own. It replaces the OFX
`net.sf.cimg.CImgBilateral`, `CImgBilateralGuided`, `CImgGuided` and `CImgRollingGuidance`, whose
range sigma and epsilon assumed a [0, 1] range. Iterations cover the rolling guidance filter.

## Range key

Edges are judged in stops, so the node behaves the same on a dark and a bright plate, and
scaling the input by 100 scales the output by 100.

- **Luminance** `l` of a pixel is Rec.709 (0.2126 R + 0.7152 G + 0.0722 B) when R, G and B are all
  processed, with alpha weighing 0; otherwise it is the mean of the processed channels.
- **Key** `k = log2(max(l, 2^-16))`. Zero, negatives and NaN count as `2^-16`; an infinity is
  capped at the largest float. A key difference is a difference in stops.
- NaNs and infinities in the image count as 0 when averaged.

## Bilateral mode

For the pixel at `p` and a neighbour `q`, with `d` the distance in pixels, `Size` the spatial
sigma `sigma_s` (times the render scale) and `Range Sigma` the range sigma `sigma_r`:

    w(q) = exp(-d^2 / (2 sigma_s^2)) * exp(-(k(q) - k(p))^2 / (2 sigma_r^2))
    out(p) = sum w(q) * image(q) / sum w(q)

The sum runs over the pixels inside the disc of radius `ceil(3 * sigma_s)` that lie inside the
region; beyond the edge of the image the window holds only the pixels inside. The cost grows with
the square of Size.

## Guided mode

He et al.'s guided filter, with the key as the guide `I = k` and the image `P` fitted per channel:

    a = (mean(I * P) - mean(I) * mean(P)) / (max(mean(I^2) - mean(I)^2, 0) + eps)
    b = mean(P) - a * mean(I)
    out = mean(a) * I + mean(b)

The means are box means of radius `max(1, round(sigma_s * sqrt(1.5)))`, taken as two separable
passes, so the cost grows only linearly with Size and not with the luminance. Two box means
spread as far as a Gaussian of `sigma_s`. `eps = Range Sigma^2`, in stops squared, so the
regularisation is scale-invariant. The box means cover only the pixels inside the buffer.

## Guide input

The optional Guide input supplies the key in place of the image's own (a joint, or cross, filter):
its luminance, from the same channel weights, decides which neighbours are alike. It is read over
the same region as the Source, and reads 0 where it has no pixel. Without a Guide connected, both
modes guide by the image itself (self-guided).

## Iterations

Each iteration filters the previous result. With a Guide connected, every iteration compares the
Guide's luminance; otherwise it compares the previous result's, so the key is rebuilt each time.
This is how CImg's iterations and the rolling guidance filter work.

## Knobs

| Parameter | Label | Default | Meaning |
|---|---|---|---|
| `mode` | Mode | Bilateral | Bilateral or Guided |
| `size` | Size | 3 | The spatial standard deviation in pixels, 0 to 50 (slider to 20). 0 leaves the input unchanged |
| `rangeSigma` | Range Sigma | 1 | How different two luminances may be and still be averaged, in stops (log2); slider to 4. Neighbours a Range Sigma apart weigh `exp(-1/2)` of an equal one. 0 leaves the input unchanged |
| `iterations` | Iterations | 1 | How many times the filter runs, 1 to 10 (slider to 5) |

The standard channel, mask and mix knobs follow. Inputs: Source, an optional Mask and an optional
Guide. A Size or Range Sigma of 0 is the identity.

## Region

The Source and the Guide are read over the render window grown by the radius (the disc radius in
Bilateral mode, the box radius in Guided mode) once per iteration, clipped to the region of
definition. A pixel does not depend on the tile it is rendered in.

## GPU

- **Bilateral mode** is a tiled direct gather up to radius 8 (`kBilateralGpuTiledMaxRadius`) and
  reads the device buffers straight above that. The GPU path covers radius up to 64
  (`Bilateral::kBilateralGpuMaxRadius`), that is Size up to about 21; a larger radius leaves the
  GPU recorder out and the CPU renders it.
- **Guided mode** runs the same separable box means on the GPU, with no radius cap.
- **Limits.** Size is at most 50 and Iterations at most 10 in the knobs, in both modes.
- **Iterations** stay resident on the device.
- **Tolerance.** 1e-4 relative against the CPU, because the weights and the key are transcendental.
- The GPU weight scales with the radius times the iterations.

The CPU properties are in `Tests/Native/NativeBilateral_Test.cpp`: a flat region is smoothed, an
edge above Range Sigma survives, the output scales with the input, and a self-guided result is
close to Bilateral's on an edge. The GPU parity is in `Tests/Native/NativeBilateralGpu_Test.cpp`.
