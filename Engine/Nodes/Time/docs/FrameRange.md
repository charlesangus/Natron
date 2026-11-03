# FrameRange

Sets the output frame range to `[firstFrame, lastFrame]` (a last frame below the first gives the
single frame `firstFrame`). Inside the range the input passes through at the same time. Outside
it, `before` and `after` choose what is shown:

- **Original**: the input at the same time, outside the range.
- **Hold**: the range's nearest end.
- **Loop**: the range repeated, `first + ((t - first) mod n)` over its `n` frames.
- **Bounce**: the range played forwards then backwards, with period `2n - 2`.
- **Black**: an empty frame (a black route; nothing is rendered).

Loop and Bounce wrap the truncated whole frame, as Read's before/after do. The node only routes
and is the identity of its input at the routed time. `reset` sets the range to the input's frame
range.

| Parameter | Default | Meaning |
|---|---|---|
| `firstFrame` | 1 | First frame of the output range |
| `lastFrame` | 100 | Last frame of the output range |
| `reset` | (button) | Set the range to the input's |
| `before` | Black | What frames before the range show |
| `after` | Black | What frames after the range show |

Port audit (every OFX and Nuke control):

| OFX (net.sf.openfx.FrameRange) | Nuke (FrameRange) | Natron knob | Default | Status |
|---|---|---|---|---|
| `frameRange` (2D int, default 1, 1) | `first_frame` (1), `last_frame` (100) | `firstFrame`, `lastFrame` | 1, 100 | mapped, Nuke's split and defaults |
| `reset` | `reset` | `reset` | | mapped |
| `before` | (none) | `before` | Black | mapped |
| `after` | (none) | `after` | Black | mapped |

Differences: OFX sets the range from the input when it is first connected; here the range keeps
its defaults until `reset`, as in Nuke. OFX's Bounce option shares Loop's id, so it never bounced;
here it has its own id.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| FrameRange (FrameRange) | exact | (route checks, golden bit-for-bit) | Routes only; Black is a black route |
