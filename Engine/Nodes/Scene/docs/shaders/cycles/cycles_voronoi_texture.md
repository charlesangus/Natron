# Voronoi Texture

Cycles texture `cycles_voronoi_texture`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| distance | float |
| color | color |
| position | point |
| w | float |
| radius | float |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Dimensions | `dimensions` | enum | 3D | knob only | Dimensions Options: 1D, 2D, 3D, 4D. |
| Metric | `metric` | enum | euclidean | knob only | Distance Metric Options: euclidean, manhattan, chebychev, minkowski. |
| Feature | `feature` | enum | f1 | knob only | Feature Options: f1, f2, smooth_f1, distance_to_edge, n_sphere_radius. |
| Use Normalize | `use_normalize` | boolean | off | knob only | Normalize |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Vector | `vector` | point | 0 0 0 | port | Vector |
| W | `w` | float | 0 | port | W |
| Scale | `scale` | float | 5 | port | Scale |
| Detail | `detail` | float | 0 | port | Detail |
| Roughness | `roughness` | float | 0.5 | port | Roughness |
| Lacunarity | `lacunarity` | float | 2 | port | Lacunarity |
| Smoothness | `smoothness` | float | 5 | port | Smoothness |
| Exponent | `exponent` | float | 0.5 | port | Exponent |
| Randomness | `randomness` | float | 1 | port | Randomness |

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
