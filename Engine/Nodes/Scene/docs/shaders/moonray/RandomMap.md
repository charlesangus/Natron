# RandomMap

MoonRay map `RandomMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Input | `input` | Rgb | 1 1 1 | port | the input color used as a base seed to generate the random value |
| Seed | `seed` | Int | 0 | knob only | additional seed added to input for random number generator |
| Monochrome | `monochrome` | Bool | on | knob only | outputs the same color for all three channels |
| Output Min | `output_min` | Float | 0 | knob only | the minimum output random value |
| Output Max | `output_max` | Float | 1 | knob only | the maximum output random value |
