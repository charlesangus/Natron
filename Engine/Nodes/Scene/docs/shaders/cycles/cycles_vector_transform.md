# Vector Transform

Cycles utility `cycles_vector_transform`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| vector | vector |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Transform Type | `transform_type` | enum | vector | knob only | Type Options: vector, point, normal. |
| Convert From | `convert_from` | enum | world | knob only | Convert From Options: world, object, camera. |
| Convert To | `convert_to` | enum | object | knob only | Convert To Options: world, object, camera. |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Vector | `vector` | vector | 0 0 0 | port | Vector |
