# RGB Curves

Cycles utility `cycles_rgb_curves`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| value | color |

## Curve

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Curves | `curves` | array_color | empty | knob only | Curves |
| Min X | `min_x` | float | 0 | knob only | Min X |
| Max X | `max_x` | float | 1 | knob only | Max X |
| Extrapolate | `extrapolate` | boolean | on | knob only | Extrapolate |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Fac | `fac` | float | 0 | port | Fac |
| Value | `value` | color | 0 0 0 | port | Color |
