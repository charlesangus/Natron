# ColorCorrectContrastMap

MoonRay map `ColorCorrectContrastMap`, for the moonray target.

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
| Contrast | `contrast` | Float | 0 | port | negative values reduce the difference in luminance towards grey and positive values increase the difference in luminance |
| Use Per Channel Contrast | `use_per_channel_contrast` | Bool | off | knob only | enables separate RGB controls for contrast |
| Contrast R | `contrast_r` | Float | 0 | port | negative values reduce the difference in luminance towards grey and positive values increase the difference in luminance for the red channel |
| Contrast G | `contrast_g` | Float | 0 | port | negative values reduce the difference in luminance towards grey and positive values increase the difference in luminance for the green channel |
| Contrast B | `contrast_b` | Float | 0 | port | negative values reduce the difference in luminance towards grey and positive values increase the difference in luminance for the blue channel |
