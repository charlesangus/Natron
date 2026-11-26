# Raycast

Cycles utility `cycles_raycast`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| is_hit | float |
| is_self_hit | float |
| hit_distance | float |
| hit_position | point |
| hit_normal | normal |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Only Local | `only_local` | boolean | off | knob only | Only Local |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Position | `position` | point | 0 0 0 | port | Position |
| Direction | `direction` | normal | 0 0 0 | port | Direction |
| Length | `length` | float | 1 | port | Length |
