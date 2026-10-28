# NoTimeBlur

Rounds the time it is asked for, so nodes downstream that sample fractional times (TimeBlur,
Retime, TimeWarp) read whole frames from upstream. It is a route: the output at `t` is the input
at `rounding(t)`, and it renders nothing. Without an input the output is black.

| Parameter | Default | Meaning |
|---|---|---|
| `rounding` | Round | Round (to nearest), Floor, Ceil or None |

Port audit (every OFX and Nuke control):

| OFX (net.sf.openfx.NoTimeBlurPlugin) | Nuke (NoTimeBlur) | Natron knob | Default | Status |
|---|---|---|---|---|
| `rounding` rint / floor / ceil / none | `rounding` | `rounding` Round / Floor / Ceil / None | Round | mapped; option ids stay rint, floor, ceil, none |
| (none) | `single` (use single frame) | (none) | | N/A: each requested time is rounded on its own, so there is no frame to share |

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| NoTimeBlur (NoTimeBlur) | exact | (route) | The input's pixels at the rounded time |
