# NoTimeBlur

Rounds the time it is asked for, so nodes downstream that sample fractional times (TimeBlur,
Retime, TimeWarp) read whole frames from upstream. It is a route: the output at `t` is the input
at `rounding(t)`, and it renders nothing. Without an input the output is black.

| Parameter | Default | Meaning |
|---|---|---|
| `rounding` | Round | Round (to nearest), Floor, Ceil or None |

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| NoTimeBlur (NoTimeBlur) | exact | (route) | The input's pixels at the rounded time |
