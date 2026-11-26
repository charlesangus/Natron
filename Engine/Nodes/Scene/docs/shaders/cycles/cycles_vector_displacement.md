# Vector Displacement

Cycles displacement `cycles_vector_displacement`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| displacement | vector |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Space | `space` | enum | tangent | knob only | Space Options: tangent, object, world. |
| Attribute | `attribute` | string |  | knob only | Attribute |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Vector | `vector` | color | 0 0 0 | port | Vector |
| Midlevel | `midlevel` | float | 0 | port | Midlevel |
| Scale | `scale` | float | 1 | port | Scale |
