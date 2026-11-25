# Float Curve

Cycles utility `cycles_float_curve`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| value | float |

## Curve

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Curve | `curve` | array_float | empty | knob only | Curve |
| Min X | `min_x` | float | 0 | knob only | Min X |
| Max X | `max_x` | float | 1 | knob only | Max X |
| Extrapolate | `extrapolate` | boolean | on | knob only | Extrapolate |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Fac | `fac` | float | 0 | port | Factor |
| Value | `value` | float | 0 | port | Value |
