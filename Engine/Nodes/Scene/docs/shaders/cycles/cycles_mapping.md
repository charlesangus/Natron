# Mapping

Cycles utility `cycles_mapping`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| vector | point |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Mapping Type | `mapping_type` | enum | point | knob only | Type Options: point, texture, vector, normal. |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Vector | `vector` | point | 0 0 0 | port | Vector |
| Location | `location` | point | 0 0 0 | port | Location |
| Rotation | `rotation` | point | 0 0 0 | port | Rotation |
| Scale | `scale` | point | 1 1 1 | port | Scale |
