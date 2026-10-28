# TimeBlur

Averages the input over a shutter: `divisions` samples, evenly spread from the shutter's
opening, the closing excluded. The shutter is `shutter` frames long and opens at `t` (Start),
`t - shutter / 2` (Centered), `t - shutter` (End) or `t + shutterCustomOffset` (Custom). A shutter
of 0 or a single division reads the input at the opening. Without an input the output is black.

Samples are read at fractional times, so a NoTimeBlur upstream collapses them to whole frames.
The blend is the shared time-sampled render (TimeWarp's and Retime's): the `timeblend` kernel on
the colour plane, with up to 8 frames besides the output's own. Fractional sample times render on
the CPU, because a region holds only whole-frame edges. The output frame range is the input's.

| Parameter | Default | Meaning |
|---|---|---|
| `divisions` | 10 | Samples along the shutter; at least 1 |
| `shutter` | 0.5 | Shutter length in frames |
| `shutterOffset` | Start | Centered, Start, End or Custom |
| `shutterCustomOffset` | 0 | Opening relative to the current frame, with Custom |

Port audit (every OFX and Nuke control):

| OFX (net.sf.openfx.TimeBlur) | Nuke (TimeBlur) | Natron knob | Default | Status |
|---|---|---|---|---|
| `division` | `divisions` | `divisions` | 10 | mapped |
| `shutter` | `shutter` | `shutter` | 0.5 | mapped |
| `shutterOffset` | `shutteroffset` | `shutterOffset` | Start | mapped |
| `shutterCustomOffset` | `shuttercustomoffset` | `shutterCustomOffset` | 0 | mapped |

Differences: no mask or mix, as in OFX.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| TimeBlur (TimeBlur) | arithmetic | `1e-12` absolute | Sample times |
| TimeBlur (TimeBlur) | arithmetic | `1e-6` absolute | CPU mean of the samples |
| TimeBlur (TimeBlur) | arithmetic | `ParityTolerance::make(1e-5, 0.)` | GPU blend of whole-frame samples against CPU |
