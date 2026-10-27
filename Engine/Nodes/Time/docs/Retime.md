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
3. **Reverse.** `s = inputFirst + advance`, or `inputLast - advance` with `reverse`, which
   animates and is taken at the output frame.
4. **Warp.** On the Time Warp tab, the curve maps the position of `s` in the input range, 0 to 1,
   to a new position: `s = inputFirst + length * warp((s - inputFirst) / length)`. The default
   curve is the identity.
5. **Filter.** Except with Box, a whole source frame (within 1e-6) reads that frame. Otherwise
   Nearest reads `floor(s + 0.5)`, Fractional reads the input at `s` itself, and Blend (the
   default) reads `floor(s)` and `floor(s) + 1` and mixes them as `(b - a) * w + a` with
   `w = s - floor(s)`.
6. **Box.** The output frame `t` covers the output span `t ± shutter / 2`. Its ends go through
   steps 2 to 4 like `t` (unwrapped, so a Loop or Bounce seam does not widen it), and the input
   span `[low, high]` they and `s` reach is averaged: input frame `f` covers `[f - 0.5, f + 0.5)`
   and weighs its overlap with the span. A frame held by Hold does not move, so its span is
   empty. While `s` is inside the input range the span stays within half a frame of it, and it
   never reaches further than 127.5 frames from `s`. A span covering one frame (speed 1 and
   shutter 1 at a whole frame, or shutter 0) reads that frame, or `floor(s + 0.5)` when the span
   is empty.

Blend, and Box over several frames, are the node's only renders, the weighted mean of the frames
read; every other case is a route, the identity of the input at the source time. The mean runs on
the GPU through the `timeblend` kernel, which folds N frames with per-frame weights (each step's
blend is its weight over the running sum), on the colour plane only, for at most 8 frames other
than the output's own. Other planes, more frames, and a mean that reads a frame where the input
has nothing (an empty region of definition, as a Black route gives) render on the CPU, where that
frame reads as black. The output frame range is `[floor(outputFirst), ceil(outputLast)]`.

| Parameter | Default | Meaning |
|---|---|---|
| `inputFirst`, `inputFirstLock` | 1, off | First input frame used, when locked |
| `inputLast`, `inputLastLock` | 100, off | Last input frame used, when locked |
| `reverse` | off | Play the input range backwards; animates |
| `outputFirst`, `outputFirstLock` | 1, off | First output frame, when locked |
| `outputLast`, `outputLastLock` | 100, off | Last output frame, when locked |
| `speed` | 1 | Input frames per output frame; animates |
| `before`, `after` | Hold | What frames outside the output range read |
| `filter` | Blend | Nearest, Blend, Fractional or Box |
| `shutter` | 1 | Box: output frames each output frame averages over; animates |
| `warp` | identity | Time Warp tab: remaps the position in the input range |

Port audit (every OFX and Nuke control):

| OFX (net.sf.openfx.Retime) | Nuke (Retime) | Natron knob | Default | Status |
|---|---|---|---|---|
| (none) | `input.first`, `input.first_lock` | `inputFirst`, `inputFirstLock` | 1, off | mapped |
| (none) | `input.last`, `input.last_lock` | `inputLast`, `inputLastLock` | 100, off | mapped |
| `reverseInput` | `reverse` | `reverse` | off | mapped; animates, as in OFX |
| (none) | `output.first`, `output.first_lock` | `outputFirst`, `outputFirstLock` | 1, off | mapped |
| (none) | `output.last`, `output.last_lock` | `outputLast`, `outputLastLock` | 100, off | mapped |
| `speed` | `speed` | `speed` | 1 | mapped |
| (none) | `before`, `after` | `before`, `after` | Hold | mapped |
| `filter` none / nearest / linear / box | `filter` none / nearest / box | `filter` Fractional / Nearest / Blend / Box | Blend | mapped |
| `warp` (Time Warp page) | `warp` (TimeWarp tab) | `warp` | identity | mapped |
| `duration` (hosts without parametric params) | (none) | (none) | | N/A: Natron has parametric params |
| (none; OFX's box option, commented out, names a shutter) | `shutter` | `shutter` | 1 | mapped |

Differences: OFX integrates the speed from the input's first frame and has no ranges or locks;
the integral here starts at `outputFirst`, as the output range does. Nuke's "none" filter is
Fractional and OFX's "linear" is Blend. The default filter is OFX's (Blend); Nuke's is Box.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Retime (Retime) | arithmetic | `1e-6` absolute | CPU blend against `(b - a) * w + a` in float |
| Retime (Retime) | arithmetic | `1e-6` absolute | CPU Box against the overlap-weighted mean in float |
| Retime (Retime) | arithmetic | `ParityTolerance::make(1e-6, 0.)` | GPU blend and Box against CPU |
| Retime (Retime) | exact | (golden bit-for-bit) | Routes are the input's pixels |
