# FrameHold

Holds an input frame. With `increment` 0 every output frame shows input frame `firstFrame`. With
an increment `n`, output frame `t` shows `firstFrame + floor((t - firstFrame) / n) * n`: every
multiple of `n` past `firstFrame` is held for the `n` frames from it. The node
only routes and is the identity of its input at the held time.

With `roundingMode` Whole Frames the held time is rounded down to a whole frame; Fractional keeps
a fractional `firstFrame`. `setToCurrentFrame` copies the timeline's current frame into
`firstFrame`. The output frame range is the input's. Unconnected, the output is black.

| Parameter | Default | Meaning |
|---|---|---|
| `roundingMode` | Whole Frames | Whole Frames rounds the held time down; Fractional keeps it |
| `firstFrame` | 1 | The frame held, or the first of the held frames with an increment |
| `setToCurrentFrame` | (button) | Set `firstFrame` to the current frame |
| `increment` | 0 | Hold every `increment`-th frame for `increment` frames; 0 holds one frame |

Port audit (every OFX and Nuke control):

| OFX (net.sf.openfx.FrameHold) | Nuke (FrameHold) | Natron knob | Default | Status |
|---|---|---|---|---|
| `firstFrame` (default 0) | `firstFrame` (default 1) | `firstFrame` | 1 | mapped, Nuke's default |
| `increment` | `increment` | `increment` | 0 | mapped; at least 0 (OFX's negative increment is dropped) |
| (none) | `setToCurrentFrame` | `setToCurrentFrame` | | mapped |
| (none) | `rounding_mode` | `roundingMode` | Whole Frames | mapped |
| sublabel (secret) | (none) | (none) | | N/A: host label detail |
| (none) | path mask (3D scene paths) | (none) | | N/A: no 3D scene graph on this input |

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| FrameHold (FrameHold) | exact | (route checks, golden bit-for-bit) | Routes only; pixels are the input's |
