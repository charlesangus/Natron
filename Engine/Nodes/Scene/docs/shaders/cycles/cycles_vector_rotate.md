# Vector Rotate

Cycles utility `cycles_vector_rotate`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| vector | vector |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Rotate Type | `rotate_type` | enum | axis | knob only | Type Options: axis, x_axis, y_axis, z_axis, euler_xyz. |
| Invert | `invert` | boolean | off | knob only | Invert |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Vector | `vector` | vector | 0 0 0 | port | Vector |
| Rotation | `rotation` | point | 0 0 0 | port | Rotation |
| Center | `center` | point | 0 0 0 | port | Center |
| Axis | `axis` | vector | 0 0 1 | port | Axis |
| Angle | `angle` | float | 0 | port | Angle |
