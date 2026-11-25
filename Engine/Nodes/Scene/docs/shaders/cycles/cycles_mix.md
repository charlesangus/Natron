# Mix

Cycles utility `cycles_mix`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| color | color |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Mix Type | `mix_type` | enum | mix | knob only | Type Options: mix, add, multiply, subtract, screen, divide, difference, darken, lighten, overlay, dodge, burn, hue, saturation, value, color, soft_light, linear_light, exclusion. |
| Use Clamp | `use_clamp` | boolean | off | knob only | Use Clamp |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Fac | `fac` | float | 0.5 | port | Fac |
| Color 1 | `color1` | color | 0 0 0 | port | Color1 |
| Color 2 | `color2` | color | 0 0 0 | port | Color2 |
