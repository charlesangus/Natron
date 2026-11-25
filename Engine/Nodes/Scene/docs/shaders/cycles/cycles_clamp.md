# Clamp

Cycles utility `cycles_clamp`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| result | float |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Clamp Type | `clamp_type` | enum | minmax | knob only | Type Options: minmax, range. |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Value | `value` | float | 1 | port | Value |
| Min | `min` | float | 0 | port | Min |
| Max | `max` | float | 1 | port | Max |
