# Median

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Median | selection | `ParityTolerance::exact()` | GPU against CPU, bit for bit, for the plain render and the special values (NaN, infinities, denormals, signed zeros): a median is a selection among input values, so both sides pick the same float. The mask, mix and channel cases compare at `arithmetic()`, because the finish pass blends; a fused Grade chain compares at `transcendental()` |

Replaces each pixel, channel by channel, by the median of a square window around it. It replaces
the OFX `net.sf.cimg.CImgMedian`. Impulse noise (isolated bright or dark pixels) vanishes while
edges stay sharp.

## Model

For a processed channel, with radius `r = floor(Size * render scale)` pixels, the window is the
`(2r + 1) x (2r + 1)` pixels centred on the pixel. The output is the median of its
`(2r + 1)^2` values, which is the `(2r + 1)^2 / 2`-th smallest (integer division, zero based), so
it is always one of the input values. Nothing is averaged and nothing is clamped: values below 0
and above 1 go through.

- **Edges.** Beyond the edge of the region of definition the window repeats the nearest pixel. Where
  the region holds no source pixel it reads 0. The region of definition is not grown.
- **Ordering.** Values are ordered on an integer key with the order of a float that is not NaN, so
  denormals stay ordered where a thread flushes them, and the CPU and the GPU select the same
  value. A NaN sample counts as 0 and -0 as +0, so every window holds `(2r + 1)^2` ordered values.
- **Threshold.** With `t = Threshold` and the median `m`, the pixel `v` is kept when
  `|v - m| <= t * max(|m|, 1e-6)` and replaced by `m` otherwise. The test is relative to the
  median, so a dark and a bright plate, or an image and the same image times 100, are treated alike
  (CImg's threshold was in intensity units, meaningless on scene-linear data). At 0 every pixel is
  replaced. A NaN source value is always replaced.

Size 0 is the identity.

## Knobs

| Parameter | Label | Default | Meaning |
|---|---|---|---|
| `size` | Size | 1 | Radius of the window in pixels, 0 to 100 (slider to 10); the window is `2 * Size + 1` pixels a side. 0 leaves the input unchanged |
| `threshold` | Threshold | 0 | A pixel is replaced only where it differs from the median by more than `Threshold` times the median. 0 replaces every pixel |

The standard channel, mask and mix knobs follow. Inputs: Source and an optional Mask.

## Region

The Source is read over the render window grown by the radius on every side, clipped to the
region of definition. A pixel does not depend on the tile it is rendered in. The CPU is one gather
pass over that region, selecting with `std::nth_element` on a reused per-thread buffer.

## GPU

- **Radius 0 to 2** sorts the window in registers (the direct kernel, `median_direct`).
- **Radius 3 to 16** stages a shared-memory tile and selects per pixel (`median_tiled`).
- **Cap.** The GPU path covers radius up to 16 (`Median::kMedianGpuMaxRadius`). A larger radius
  leaves the GPU recorder out and the CPU renders it.
- **Exactness.** Selection on the same integer keys gives a result equal to the CPU's bit for bit,
  at every radius and threshold. There is no float tolerance to state.
- The GPU weight scales with the radius in pixels.

The CPU properties are in `Tests/Native/NativeMedian_Test.cpp`: impulses vanish at size 1, a step
edge survives, size 0 is the identity, the threshold acts the same at 1x and 100x scale, and tiles
equal the full frame. The GPU parity is in `Tests/Native/NativeMedianGpu_Test.cpp`.
