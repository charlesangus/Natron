# FrameBlend

Blends a range of frames of the input into each output frame. The range runs from `startFrame`
to `endFrame`, offset by the current frame unless `absolute` is on. `endFrame` is always read,
then every `frameInterval`-th frame back towards `startFrame`: `n = max(1, (|end - start| + 1) /
frameInterval)` frames, folded oldest first. Without an input the output is black.

1. **Fold.** The result starts as the oldest frame and takes in each later one. Average is the
   weighted mean, frame `i` weighing `f^(n - 1 - i)`. Min, Max, Sum and Product first scale the
   result by `f`, then take the minimum, maximum, sum or product with the new frame. Over lays
   the new frame over the scaled result, `in + (1 - a) * f * r`, with `a` the new frame's alpha.
   `f = (1 - decay)^frameInterval`. Pixels outside a frame read zero.
2. **Output Count.** Writes `n` to alpha.
3. **Stages.** Mask, mix and unprocessed channels blend against the input at the current frame.

One frame read, with Output Count off, is a route to that frame when it is the current one or
nothing masks or mixes; the node then renders nothing. Frames are fetched 8 at a time on the CPU.
On the GPU the fold runs through the `timeblend` kernel on the colour plane. Up to 8 frames other
than the output's own stay on the GPU; more, a frame where the input has nothing, or Output
Count render on the CPU. The output frame range is the input's.

| Parameter | Default | Meaning |
|---|---|---|
| `startFrame` | -5 | First frame of the range; relative unless `absolute` |
| `endFrame` | 0 | Last frame of the range, always read |
| `absolute` | off | The range is in absolute frames |
| `inputRange` | (button) | Sets `absolute` and the range to the input's frame range |
| `frameInterval` | 1 | Reads every n-th frame back from `endFrame`; at least 1 |
| `operation` | Average | Average, Min, Max, Sum, Product or Over |
| `decay` | 0 | Fade of older frames, per frame |
| `outputCount` | off | Writes the number of frames read to alpha |
| `channels`, `maskInvert`, `mix` | RGBA, off, 1 | Stage knobs |

Port audit (every OFX and Nuke control):

| OFX (net.sf.openfx.FrameBlend) | Nuke (FrameBlend) | Natron knob | Default | Status |
|---|---|---|---|---|
| `frameRange` (2 ints) | `startframe`, `endframe` | `startFrame`, `endFrame` | -5, 0 | mapped; two knobs |
| `absolute` | `userange` (Custom) | `absolute` | off | mapped |
| (none) | `numframes` | `startFrame` | -5 | mapped: n frames is `startFrame = 1 - n`, `endFrame = 0` |
| `inputRange` | `getinputrange` | `inputRange` | | mapped |
| `frameInterval` | (none) | `frameInterval` | 1 | mapped |
| `operation` | (none) | `operation` | Average | mapped; all six kept |
| `decay` | (none) | `decay` | 0 | mapped |
| `outputCount` | output Image count to | `outputCount` | off | mapped; to alpha only |
| `processR/G/B/A` | `channels` | `channels` | RGBA | mapped |
| FgM input (`inputmask`) | Foreground matte | Mask input | | mapped to the host mask |
| `maskApply`, `maskInvert`, `mix` | `mask`, `invert_mask`, `mix` | Mask input, `maskInvert`, `mix` | | mapped |
| (none) | `inject`, `fringe` | (none) | | N/A: no native node has them |

Differences: OFX reads no frame when the range is shorter than the interval; here `endFrame` is
always read. The fold starts from the oldest frame rather than black, so Product decays one step
less than OFX when `decay` is not 0. OFX lists frames it never reads in its frames needed; here
the frames needed are the frames read. The knobs are camelCase, after their labels.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| FrameBlend (FrameBlend) | arithmetic | `1e-5` / `1e-6` absolute | CPU fold against a double reference |
| FrameBlend (FrameBlend) | arithmetic | `ParityTolerance::make(1e-5, 0.)` | GPU fold against CPU; the arithmetic is the same |
| FrameBlend (FrameBlend) | exact | (golden bit-for-bit) | Routes are the input's pixels |
