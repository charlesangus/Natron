# Color Correct HSV Map

MoonRay map `ColorCorrectHsvMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Input | `input` | Rgb | 1 1 1 | port | input color |
| On | `on` | Bool | on | knob only | all attributes on/off |
| Hue Shift | `hue_shift` | Float | 0 | port | shifts the hue of the input (360 rolls over back to 0) |
| Saturation Factor | `saturation_factor` | Float | 1 | port | multiplies the saturation of the input |
| Saturation Contrast | `saturation_contrast` | Float | 0 | knob only | modifies the contrast of the input's saturation (-1, 1) |
| Saturation Shift | `saturation_shift` | Float | 0 | knob only | shifts the saturation of the input (-1, 1) |
| Value Factor | `value_factor` | Float | 1 | port | multiplies the value of the input |
| Value Contrast | `value_contrast` | Float | 0 | knob only | modifies the contrast of the input's value (-1, 1) |
| Value Shift | `value_shift` | Float | 0 | knob only | shifts the value of the input (-1, 1) |
| Clamp | `clamp` | Bool | off | knob only | clamps output to [0,1] range |
