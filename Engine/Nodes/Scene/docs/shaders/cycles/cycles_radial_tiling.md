# Radial Tiling

Cycles utility `cycles_radial_tiling`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| segment_coordinates | point |
| segment_id | float |
| max_unit_parameter | float |
| x_axis_A_angle_bisector | float |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Use Normalize | `use_normalize` | boolean | off | knob only | Normalize |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Vector | `vector` | point | 0 0 0 | port | Vector |
| R Gon Sides | `r_gon_sides` | float | 5 | port | Sides |
| R Gon Roundness | `r_gon_roundness` | float | 0 | port | Roundness |
