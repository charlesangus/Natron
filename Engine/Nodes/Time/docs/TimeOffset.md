# TimeOffset

Moves the input in time: output frame `t` shows input frame `t - timeOffset`, so input frame `f`
appears at `f + timeOffset`. The node only routes; it renders nothing of its own and is the
identity of its input at the routed time.

1. **Offset.** `s = t - timeOffset`, the offset taken at `t`; it animates.
2. **Reverse.** With `reverseInput`, and a bounded input frame range `[first, last]`, the
   offset input is played backwards: `s = first + last - s`.
3. **Clip.** With `clipToInputRange`, and a bounded input range, `s` is clamped to it.
4. **Round.** With `roundingMode` Whole Frames, `s` is rounded down to a whole frame;
   Fractional keeps it.

An unbounded input range (a generator without a frame range) makes reverse and clip no-ops. The
output frame range is the input's, shifted by the offset. With an animated offset each end is the
output frame that shows that end of the input, `t` with `t - timeOffset(t)` at the end. Unconnected,
the output is black.

| Parameter | Default | Meaning |
|---|---|---|
| `roundingMode` | Whole Frames | Whole Frames rounds the source time down; Fractional keeps it |
| `timeOffset` | 0 | Frames the input moves forward; animates |
| `reverseInput` | off | Play the input range backwards |
| `clipToInputRange` | off | Never read outside the input range |

Differences: OFX's offset does not animate; here it does.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| TimeOffset (TimeOffset) | exact | (route checks, golden bit-for-bit) | Routes only; pixels are the input's |
