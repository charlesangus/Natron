# Brick Texture

Cycles texture `cycles_brick_texture`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| color | color |
| fac | float |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Offset | `offset` | float | 0.5 | knob only | Offset |
| Offset Frequency | `offset_frequency` | int | 2 | knob only | Offset Frequency |
| Squash | `squash` | float | 1 | knob only | Squash |
| Squash Frequency | `squash_frequency` | int | 2 | knob only | Squash Frequency |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Vector | `vector` | point | 0 0 0 | port | Vector |
| Color 1 | `color1` | color | 0 0 0 | port | Color1 |
| Color 2 | `color2` | color | 0 0 0 | port | Color2 |
| Mortar | `mortar` | color | 0 0 0 | port | Mortar |
| Scale | `scale` | float | 5 | port | Scale |
| Mortar Size | `mortar_size` | float | 0.02 | port | Mortar Size |
| Mortar Smooth | `mortar_smooth` | float | 0 | port | Mortar Smooth |
| Bias | `bias` | float | 0 | port | Bias |
| Brick Width | `brick_width` | float | 0.5 | port | Brick Width |
| Row Height | `row_height` | float | 0.25 | port | Row Height |

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
