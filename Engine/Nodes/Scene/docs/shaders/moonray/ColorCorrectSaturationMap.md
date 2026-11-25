# ColorCorrectSaturationMap

MoonRay map `ColorCorrectSaturationMap`, for the moonray target.

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
| Saturation | `saturation` | Float | 1 | port | desaturates the input below 1.0 and adds saturation above 1.0 |
| Use Per Channel Saturation | `use_per_channel_saturation` | Bool | off | knob only | enables separate RGB controls for saturation |
| Saturation R | `saturation_r` | Float | 1 | port | desaturates the red channel input below 1.0 and adds saturation above 1.0 |
| Saturation G | `saturation_g` | Float | 1 | port | desaturates the green channel input below 1.0 and adds saturation above 1.0 |
| Saturation B | `saturation_b` | Float | 1 | port | desaturates the blue channel input below 1.0 and adds saturation above 1.0 |
