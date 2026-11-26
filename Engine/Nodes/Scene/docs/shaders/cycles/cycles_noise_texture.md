# Noise Texture

Cycles texture `cycles_noise_texture`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| fac | float |
| color | color |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Dimensions | `dimensions` | enum | 3D | knob only | Dimensions Options: 1D, 2D, 3D, 4D. |
| Type | `type` | enum | fBM | knob only | Type Options: multifractal, fBM, hybrid_multifractal, ridged_multifractal, hetero_terrain. |
| Use Normalize | `use_normalize` | boolean | on | knob only | Normalize |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Vector | `vector` | point | 0 0 0 | port | Vector |
| W | `w` | float | 0 | port | W |
| Scale | `scale` | float | 1 | port | Scale |
| Detail | `detail` | float | 2 | port | Detail |
| Roughness | `roughness` | float | 0.5 | port | Roughness |
| Lacunarity | `lacunarity` | float | 2 | port | Lacunarity |
| Offset | `offset` | float | 0 | port | Offset |
| Gain | `gain` | float | 1 | port | Gain |
| Distortion | `distortion` | float | 0 | port | Distortion |

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
