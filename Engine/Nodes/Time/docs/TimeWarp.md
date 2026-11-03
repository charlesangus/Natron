# TimeWarp

Remaps time through a curve: output frame `t` shows input frame `s = lookup(t)`. A rising curve
plays the input forward at the curve's slope, a flat stretch holds a frame, and a falling one
plays it backwards. A new node keys `lookup` as the identity at the project's first and last
frames with cubic keys, so the curve carries on along its slope past them; a linear or constant
end key holds its value past the end instead. A loaded node keeps its saved curve.

1. **Lookup.** `s = lookup(t)`. Without an input the output is black.
2. **Filter.** The same filters as Retime: except with Box, a whole source frame (within 1e-6)
   reads that frame; otherwise Nearest reads `floor(s + 0.5)`, Fractional reads the input at `s`,
   and Blend (the default) mixes `floor(s)` and `floor(s) + 1` as `(b - a) * w + a` with
   `w = s - floor(s)`.
3. **Box.** The output span `t ± shutter / 2` goes through the curve: the input span runs from
   the lowest to the highest of `lookup` at its two ends and at `t`, and input frame `f`, covering
   `[f - 0.5, f + 0.5)`, weighs its overlap with it. A flat curve gives an empty span, which
   reads `floor(s + 0.5)`. While `s` is inside the input's frame range the span stays within half
   a frame of that range, and it never reaches further than 127.5 frames from `s`.

The renders, routes and GPU path are Retime's (the `timeblend` kernel, colour plane, at most 8
frames besides the output's own). The output frame range is the input's.

| Parameter | Default | Meaning |
|---|---|---|
| `lookup` | identity keys at the project range | Input frame shown at each output frame; animates |
| `filter` | Blend | Nearest, Blend, Fractional or Box |
| `shutter` | 1 | Box: output frames each output frame averages over; animates |

Port audit (every OFX and Nuke control):

| OFX (net.sf.openfx.Retime) | Nuke (TimeWarp) | Natron knob | Default | Status |
|---|---|---|---|---|
| `warp` (Time Warp page) | `lookup` (input frame) | `lookup` | identity | mapped; an absolute frame curve, as in Nuke, rather than OFX's 0..1 position curve |
| (none) | `length` (input length) | `shutter` | 1 | mapped; in output frames through the curve, so the slope sets the input length |
| (none) | set to df/dt | (none) | | N/A: `shutter` already follows the curve's slope |
| `filter` none / nearest / linear / box | `filter` none / nearest / box | `filter` Fractional / Nearest / Blend / Box | Blend | mapped |
| (none) | curve editor button | (none) | | N/A: the curve editor shows `lookup` |

Differences: Nuke keys only frame 1 on a new node; here the identity is keyed at both ends of the
project range so the default is a pass-through. Nuke's `length` is in input frames, while
`shutter` is in output frames mapped through the curve, which is what set to df/dt computes. The
default filter is Blend; Nuke's is Box.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| TimeWarp (TimeWarp) | arithmetic | `1e-6` absolute | CPU blend against `(b - a) * w + a` in float |
| TimeWarp (TimeWarp) | arithmetic | `ParityTolerance::make(1e-6, 0.)` | GPU blend against CPU |
| TimeWarp (TimeWarp) | exact | (golden bit-for-bit) | Routes are the input's pixels |
