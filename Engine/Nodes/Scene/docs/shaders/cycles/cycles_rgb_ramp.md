# RGB Ramp

Cycles utility `cycles_rgb_ramp`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| color | color |
| alpha | float |

## Ramp

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Ramp | `ramp` | array_color | identity | knob only | Ramp |
| Ramp Alpha | `ramp_alpha` | array_float | identity | knob only | Ramp Alpha |
| Interpolate | `interpolate` | boolean | on | knob only | Interpolate |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Fac | `fac` | float | 0 | port | Fac |
