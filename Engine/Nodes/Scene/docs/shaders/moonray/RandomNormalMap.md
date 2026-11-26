# Random Normal Map

MoonRay normal_map `RandomNormalMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | NormalMap |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Input | `input` | Rgb | 1 1 1 | port | the input color used as a base seed to generate the random value |
| Seed | `seed` | Int | 0 | knob only | additional seed added to input for random number generator |
