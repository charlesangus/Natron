# Vector Map Range

Cycles utility `cycles_vector_map_range`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| vector | vector |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Range Type | `range_type` | enum | linear | knob only | Type Options: linear, stepped, smoothstep, smootherstep. |
| Use Clamp | `use_clamp` | boolean | off | knob only | Use Clamp |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Vector | `vector` | vector | 0 0 0 | port | Vector |
| From Min | `from_min` | vector | 0 0 0 | port | From_Min_FLOAT3 |
| From Max | `from_max` | vector | 1 1 1 | port | From_Max_FLOAT3 |
| To Min | `to_min` | vector | 0 0 0 | port | To_Min_FLOAT3 |
| To Max | `to_max` | vector | 1 1 1 | port | To_Max_FLOAT3 |
| Steps | `steps` | vector | 4 4 4 | port | Steps_FLOAT3 |
