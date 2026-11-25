# ColorCorrectGainOffsetMap

MoonRay map `ColorCorrectGainOffsetMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Input | `input` | Rgb | 1 1 1 | port | bind the input here |
| On | `on` | Bool | on | knob only | enables/disables all color correct operations |
| Mix | `mix` | Float | 1 | port | how much of the overall color correct to mix in |
| Gain | `gain` | Float | 1 | port | multiplies the input channels by the specified values |
| Offset | `offset` | Float | 0 | port | adds the specified values to the input |
| Use Per Channel Gain Offset | `use_per_channel_gain_offset` | Bool | off | knob only | enables separate RGB controls for gain and offset |
| Gain R | `gain_r` | Float | 1 | port | multiplies the red channel by the specified values |
| Gain G | `gain_g` | Float | 1 | port | multiplies the green channel by the specified values |
| Gain B | `gain_b` | Float | 1 | port | multiplies the blue channel by the specified values |
| Offset R | `offset_r` | Float | 1 | port | adds the specified values to the red channel |
| Offset G | `offset_g` | Float | 1 | port | adds the specified values to the green channel |
| Offset B | `offset_b` | Float | 1 | port | adds the specified values to the blue channel |
