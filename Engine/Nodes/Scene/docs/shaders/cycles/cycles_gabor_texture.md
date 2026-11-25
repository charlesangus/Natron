# Gabor Texture

Cycles texture `cycles_gabor_texture`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| value | float |
| phase | float |
| intensity | float |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Type | `type` | enum | 2D | knob only | Type Options: 2D, 3D. |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Vector | `vector` | point | 0 0 0 | port | Vector |
| Scale | `scale` | float | 5 | port | Scale |
| Frequency | `frequency` | float | 2 | port | Frequency |
| Anisotropy | `anisotropy` | float | 1 | port | Anisotropy |
| Orientation 2 D | `orientation_2d` | float | 0.785398 | port | Orientation 2D |
| Orientation 3 D | `orientation_3d` | vector | 1.41421 1.41421 0 | port | Orientation 3D |

## Texture Mapping

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Tex Mapping Translation | `tex_mapping_translation` | point | 0 0 0 | knob only | Translation |
| Tex Mapping Rotation | `tex_mapping_rotation` | vector | 0 0 0 | knob only | Rotation |
| Tex Mapping Scale | `tex_mapping_scale` | vector | 1 1 1 | knob only | Scale |
| Tex Mapping Min | `tex_mapping_min` | vector | -3.40282e+38 -3.40282e+38 -3.40282e+38 | knob only | Min |
| Tex Mapping Max | `tex_mapping_max` | vector | 3.40282e+38 3.40282e+38 3.40282e+38 | knob only | Max |
| Tex Mapping Use Minmax | `tex_mapping_use_minmax` | boolean | off | knob only | Use Min Max |
| Tex Mapping X Mapping | `tex_mapping_x_mapping` | enum | x | knob only | x_mapping Options: none, x, y, z. |
| Tex Mapping Y Mapping | `tex_mapping_y_mapping` | enum | y | knob only | y_mapping Options: none, x, y, z. |
| Tex Mapping Z Mapping | `tex_mapping_z_mapping` | enum | z | knob only | z_mapping Options: none, x, y, z. |
| Tex Mapping Type | `tex_mapping_type` | enum | texture | knob only | Type Options: point, texture, vector, normal. |
| Tex Mapping Projection | `tex_mapping_projection` | enum | flat | knob only | Projection Options: flat, cube, tube, sphere. |
