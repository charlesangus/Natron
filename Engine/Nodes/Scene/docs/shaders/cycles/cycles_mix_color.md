# Mix Color

Cycles utility `cycles_mix_color`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| result | color |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Blend Type | `blend_type` | enum | mix | knob only | Type Options: mix, add, multiply, subtract, screen, divide, difference, darken, lighten, overlay, dodge, burn, hue, saturation, value, color, soft_light, linear_light, exclusion. |
| Use Clamp Result | `use_clamp_result` | boolean | off | knob only | Use Clamp Result |
| Use Clamp | `use_clamp` | boolean | on | knob only | Use Clamp |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Fac | `fac` | float | 0.5 | port | Factor |
| A | `a` | color | 0 0 0 | port | A |
| B | `b` | color | 0 0 0 | port | B |
