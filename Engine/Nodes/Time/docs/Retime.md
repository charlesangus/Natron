# Retime

Changes the speed of the input. The input range `[inputFirst, inputLast]` is the input's frame
range unless a lock holds an end; an unbounded input (a generator without a frame range) uses the
knob values, 1 and 100 by default. The output range follows from the locks and the speed:

- Both output ends locked: the output is `[outputFirst, outputLast]` and the speed is fixed at
  `(inputLast - inputFirst) / (outputLast - outputFirst)`; `speed` is ignored.
- Only `outputLastLock`: `outputFirst = outputLast - length / |speed(outputLast)|`.
- Otherwise: `outputFirst` is its locked value or `inputFirst`, and
  `outputLast = outputFirst + length / |speed(outputFirst)|`.

An unlocked end's knob is disabled and shows the value the node resolves it to.

1. **Outside the output range.** `before` and `after` choose what a frame outside it reads:
   Continue keeps its time (the speed carries on), Hold clamps it to the range, Loop and Bounce
   wrap it into the whole-frame range as FrameRange does, and Black shows an empty frame.
2. **Speed.** The input advances by the integral of `speed` from `outputFirst` to `t`, or by
   the fixed speed times `t - outputFirst` when both output ends are locked. `speed` animates.
3. **Reverse.** `s = inputFirst + advance`, or `inputLast - advance` with `reverse`.
4. **Warp.** On the Time Warp tab, the curve maps the position of `s` in the input range, 0 to 1,
   to a new position: `s = inputFirst + length * warp((s - inputFirst) / length)`. The default
   curve is the identity.
5. **Filter.** A whole source frame (within 1e-6) reads that frame. Otherwise Nearest reads
   `floor(s + 0.5)`, Fractional reads the input at `s` itself, and Blend (the default) reads
   `floor(s)` and `floor(s) + 1` and mixes them as `(b - a) * w + a` with `w = s - floor(s)`.

Blend is the node's only render; every other case is a route, the identity of the input at the
source time. The blend runs on the GPU through the `timeblend` kernel, which folds N frames with
per-frame weights (each step's blend is its weight over the running sum), on the colour plane only;
other planes, and frames missing on the device, render on the CPU. The output frame range is
`[floor(outputFirst), ceil(outputLast)]`.

| Parameter | Default | Meaning |
|---|---|---|
| `inputFirst`, `inputFirstLock` | 1, off | First input frame used, when locked |
| `inputLast`, `inputLastLock` | 100, off | Last input frame used, when locked |
| `reverse` | off | Play the input range backwards |
| `outputFirst`, `outputFirstLock` | 1, off | First output frame, when locked |
| `outputLast`, `outputLastLock` | 100, off | Last output frame, when locked |
| `speed` | 1 | Input frames per output frame; animates |
| `before`, `after` | Hold | What frames outside the output range read |
| `filter` | Blend | Nearest, Blend or Fractional |
| `warp` | identity | Time Warp tab: remaps the position in the input range |

Port audit (every OFX and Nuke control):

| OFX (net.sf.openfx.Retime) | Nuke (Retime) | Natron knob | Default | Status |
|---|---|---|---|---|
| (none) | `input.first`, `input.first_lock` | `inputFirst`, `inputFirstLock` | 1, off | mapped |
| (none) | `input.last`, `input.last_lock` | `inputLast`, `inputLastLock` | 100, off | mapped |
| `reverseInput` | `reverse` | `reverse` | off | mapped; not animated |
| (none) | `output.first`, `output.first_lock` | `outputFirst`, `outputFirstLock` | 1, off | mapped |
| (none) | `output.last`, `output.last_lock` | `outputLast`, `outputLastLock` | 100, off | mapped |
| `speed` | `speed` | `speed` | 1 | mapped |
| (none) | `before`, `after` | `before`, `after` | Hold | mapped |
| `filter` none / nearest / linear / box | `filter` none / nearest / box | `filter` Fractional / Nearest / Blend | Blend | mapped; box DEFERRED |
| `warp` (Time Warp page) | `warp` (TimeWarp tab) | `warp` | identity | mapped |
| `duration` (hosts without parametric params) | (none) | (none) | | N/A: Natron has parametric params |
| (none) | `shutter` | (none) | | DEFERRED with the box filter |

Differences: OFX integrates the speed from the input's first frame and has no ranges or locks;
the integral here starts at `outputFirst`, as the output range does. Nuke's "none" filter is
Fractional and OFX's "linear" is Blend.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Retime (Retime) | arithmetic | `1e-6` absolute | CPU blend against `(b - a) * w + a` in float |
| Retime (Retime) | arithmetic | `ParityTolerance::make(1e-6, 0.)` | GPU blend against CPU |
| Retime (Retime) | exact | (golden bit-for-bit) | Routes are the input's pixels |
