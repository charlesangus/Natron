# ClampMap

MoonRay map `ClampMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Input | `input` | Rgb | 1 1 1 | port | the input values to be remapped |
| Clamp | `clamp` | Bool | on | knob only | enables/disables clamping of the output values.  This useful prevent out-of-range values when expanding the input values. |
| Clamp Min | `clamp_min` | Float | 0 | knob only | the minimum value output by this map when 'clamp' is enabled |
| Clamp Max | `clamp_max` | Float | 1 | knob only | the maximum value output by this map when 'clamp' is enabled |
