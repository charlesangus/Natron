# Map Range

Cycles utility `cycles_map_range`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| result | float |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Range Type | `range_type` | enum | linear | knob only | Type Options: linear, stepped, smoothstep, smootherstep. |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Value | `value` | float | 1 | port | Value |
| From Min | `from_min` | float | 0 | port | From Min |
| From Max | `from_max` | float | 1 | port | From Max |
| To Min | `to_min` | float | 0 | port | To Min |
| To Max | `to_max` | float | 1 | port | To Max |
| Steps | `steps` | float | 4 | port | Steps |
| Clamp | `clamp` | boolean | off | port | Clamp |
