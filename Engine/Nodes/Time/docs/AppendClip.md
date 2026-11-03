# AppendClip

Plays its connected inputs one after another from `firstFrame`, each over its own frame range;
disconnected inputs are skipped. Each input overlaps the previous one by `crossDissolve` frames,
across which the previous one fades into it, but never more than two inputs show at once. Before
the first input its first frame holds and after the last its last frame does, unless `fadeIn` or
`fadeOut` asks for black there. The first `fadeIn` output frames ramp up from black and the last
`fadeOut` ones down to it. An input with an unbounded range, such as a generator, plays for the
project's range. The output frame range is `[firstFrame, lastFrame]`.

A frame showing one input at full weight is an identity of that input at its own time; a frame
showing nothing is black. Otherwise the output is `alpha0 * A + alpha1 * B`, the remaining weight
black, through the shared time blend: the `timeblend` kernel on the colour plane on the GPU.

| Parameter | Default | Meaning |
|---|---|---|
| `fadeIn` | 0 | Frames to fade in from black at the start of the first input |
| `fadeOut` | 0 | Frames to fade out to black at the end of the last input |
| `crossDissolve` | 0 | Frames each input overlaps the previous one |
| `firstFrame` | 1 | Output frame the first input starts at |
| `lastFrame` | (computed) | Last output frame, read-only and not saved |
| `updateLastFrame` | | Recomputes `lastFrame` from the inputs' current ranges |

Port audit (every OFX control):

| OFX (net.sf.openfx.AppendClip) | Natron knob | Default | Status |
|---|---|---|---|
| `fadeIn` | `fadeIn` | 0 | mapped |
| `fadeOut` | `fadeOut` | 0 | mapped |
| `crossDissolve` | `crossDissolve` | 0 | mapped |
| `firstFrame` | `firstFrame` | 1 | mapped |
| `lastFrame` | `lastFrame` | | mapped, read-only |
| `updateLastFrame` | `updateLastFrame` | | mapped, labelled "Update Last Frame" |

Differences: OFX blends a dissolve as `A + (B - A) * (1 - alpha0)` after scaling both weights by
a fade, so a fade over a dissolve dims only the first input; here both
inputs keep their faded weights and the rest is black. Without a fade the two agree. No mask or
mix, as in OFX.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| AppendClip (AppendClip) | arithmetic | `1e-5` absolute | CPU dissolve and fades |
| AppendClip (AppendClip) | arithmetic | `ParityTolerance::make(1e-5, 0.)` | GPU blend against CPU |
