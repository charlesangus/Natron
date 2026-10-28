# SlitScan

Gives each output pixel its own time and reads the Source at that time. The time is
`v * retimeGain + retimeOffset`, plus the current frame unless `retimeAbsolute` is on, where `v`
comes from `retimeFunction`: a vertical ramp from 0 at the centre of the Source's bottom row to 1
at the centre of its top row (Horizontal Slit), the same ramp across the columns (Vertical Slit),
or `retimeMapChannel` of the Retime Map input (Retime Map; 0 when the input is disconnected or
lacks the channel). Nearest reads the frame nearest the time; Linear blends the two frames around
it.

The frames read form a window, declared in `getFramesNeeded`: with a slit,
`[offset + min(0, gain), offset + max(0, gain)]`; with the map, `frameRange`; both relative to the
current frame unless absolute, rounded for Nearest and widened to whole frames for Linear. A
pixel's time is clamped to the window. The window holds at most 64 frames: a wider one keeps the
end nearest the unscaled time and the node shows a warning. A window of one frame, or a constant
time (zero gain, or the map function without a map), routes to the Source at that frame.

The output is the sum over the window of `weight * frame`, oldest first. On the GPU the `slitscan`
kernel adds one frame per dispatch on the colour plane when every frame of the window is resident
(at most 8 frames other than the current one; the default 11-frame window renders on the CPU).

| Parameter | Default | Meaning |
|---|---|---|
| `retimeFunction` | Horizontal Slit | Where each pixel's `v` comes from |
| `retimeMapChannel` | rgba.A | Channel of the Retime Map read by the Retime Map function |
| `retimeOffset` | 0 | Added to the time after the gain |
| `retimeGain` | -10 | Multiplies `v` |
| `retimeAbsolute` | off | Times are frames rather than relative to the current frame |
| `frameRange` | -10, 0 | With the map, the frames read |
| `filter` | Nearest | Nearest or Linear |

Port audit (every OFX control):

| OFX (net.sf.openfx.SlitScan) | Natron knob | Default | Status |
|---|---|---|---|
| `retimeFunction` | `retimeFunction` | Horizontal Slit | mapped |
| `retimeOffset` | `retimeOffset` | 0 | mapped |
| `retimeGain` | `retimeGain` | -10 | mapped |
| `retimeAbsolute` ("Absolute") | `retimeAbsolute` ("Retime Absolute") | off | mapped; label follows the script name |
| `frameRange` ("Max. Frame Range") | `frameRange` ("Frame Range") | -10, 0 | mapped; label follows the script name |
| `filter` | `filter` | Nearest | mapped (OFX has no Box either) |
| first component of Retime Map | `retimeMapChannel` | rgba.A | new: any channel of any layer |

Tolerances: the CPU and GPU agree within 1e-5; both sum in float in the same order.
