# Retime

Changes the speed of the input. Two mappings place the input on the output's timeline: the in
mapping puts input frame `originalIn` at output frame `newIn`, the out mapping puts `originalOut`
at `newOut`. An original end whose mapping is off is the input's frame range end (or the knob
value, 1 or 100 by default, for an input without a frame range). The new range follows from the
mappings and the speed, with `length = originalOut - originalIn`:

- In mapping only (the default): `newOut = newIn + length / |speed(newIn)|`.
- Out mapping only: `newIn = newOut - length / |speed(newOut)|`.
- Both: the speed is fixed at `length / (newOut - newIn)`, written into `speed` (unless it is
  animated or has an expression) and greyed, so the input range is squeezed or stretched to fit.
- Neither: `newIn = originalIn`, and `newOut` follows as with the in mapping.

The knobs of a mapping that is off are greyed and show the values the node resolves them to.

1. **Outside the output range.** `before` and `after` choose what a frame outside it reads:
   Continue keeps its time (the speed carries on), Hold clamps it to the range, Loop and Bounce
   wrap it into the whole-frame range as FrameRange does, and Black shows an empty frame.
2. **Speed.** The input advances by the integral of `speed` from the anchor (`newOut` with the
   out mapping alone, `newIn` otherwise) to `t`, or by the fixed speed times `t - newIn` when
   both mappings are on. `speed` animates.
3. **Reverse.** `s = originalIn + advance` (`originalOut + advance` from the `newOut` anchor).
   With `reverse`, which animates and is taken at the output frame, the other original end lands
   on the anchor and `s` runs back from it: `s = originalOut - advance`, or `originalIn - advance`
   from the `newOut` anchor.
4. **Filter.** Except with Box, a whole source frame (within 1e-6) reads that frame. Otherwise
   Nearest reads `floor(s + 0.5)`, Fractional reads the input at `s` itself, and Blend (the
   default) reads `floor(s)` and `floor(s) + 1` and mixes them as `(b - a) * w + a` with
   `w = s - floor(s)`.
5. **Box.** The output frame `t` covers the output span `t ± shutter / 2`. Its ends go through
   steps 2 and 3 like `t` (unwrapped, so a Loop or Bounce seam does not widen it), and the input
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
frame reads as black. The output frame range is `[floor(newIn), ceil(newOut)]`. The render code
is shared with TimeWarp, which remaps time through a curve instead.

| Parameter | Default | Meaning |
|---|---|---|
| `originalIn`, `newIn`, `useInMapping` | 1, 1, on | Input frame `originalIn` lands on `newIn` |
| `originalOut`, `newOut`, `useOutMapping` | 100, 100, off | Input frame `originalOut` lands on `newOut` |
| `speed` | 1 | Input frames per output frame; animates; fixed with both mappings |
| `reverse` | off | Play the input range backwards; animates |
| `before`, `after` | Hold | What frames outside the output range read |
| `filter` | Blend | Nearest, Blend, Fractional or Box |
| `shutter` | 1 | Box: output frames each output frame averages over; animates |

Port audit (every OFX and Nuke control):

| OFX (net.sf.openfx.Retime) | Nuke (Retime) | Natron knob | Default | Status |
|---|---|---|---|---|
| (none) | `input.first`, `input.first_lock` | `originalIn`, `useInMapping` | 1, on | mapped; the lock is the mapping toggle |
| (none) | `input.last`, `input.last_lock` | `originalOut`, `useOutMapping` | 100, off | mapped; the lock is the mapping toggle |
| (none) | `output.first`, `output.first_lock` | `newIn` (toggled by `useInMapping`) | 1 | mapped |
| (none) | `output.last`, `output.last_lock` | `newOut` (toggled by `useOutMapping`) | 100 | mapped |
| `speed` | `speed` | `speed` | 1 | mapped |
| `reverseInput` | `reverse` | `reverse` | off | mapped; animates, as in OFX |
| (none) | `before`, `after` | `before`, `after` | Hold | mapped |
| `filter` none / nearest / linear / box | `filter` none / nearest / box | `filter` Fractional / Nearest / Blend / Box | Blend | mapped |
| `warp` (Time Warp page) | `warp` (TimeWarp tab) | (none) | | moved: TimeWarp's `lookup` |
| `duration` (hosts without parametric params) | (none) | (none) | | N/A: Natron has parametric params |
| (none; OFX's box option, commented out, names a shutter) | `shutter` | `shutter` | 1 | mapped |

Differences: OFX integrates the speed from the input's first frame and has no ranges or locks;
the integral here starts at the anchor, as the new range does. Nuke locks the input and output
ends separately; here one toggle per end turns its whole mapping on. Nuke's "none" filter is
Fractional and OFX's "linear" is Blend. The default filter is OFX's (Blend); Nuke's is Box.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Retime (Retime) | arithmetic | `1e-6` absolute | CPU blend against `(b - a) * w + a` in float |
| Retime (Retime) | arithmetic | `1e-6` absolute | CPU Box against the overlap-weighted mean in float |
| Retime (Retime) | arithmetic | `ParityTolerance::make(1e-6, 0.)` | GPU blend and Box against CPU |
| Retime (Retime) | exact | (golden bit-for-bit) | Routes are the input's pixels |
