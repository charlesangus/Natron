# Sky Texture

Cycles texture `cycles_sky_texture`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| color | color |

## Sky

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Sky Type | `sky_type` | enum | multiple_scattering | knob only | Type Options: preetham, hosek_wilkie, single_scattering, multiple_scattering. |
| Altitude | `altitude` | float | 100 | knob only | Altitude |
| Air Density | `air_density` | float | 1 | knob only | Air |
| Aerosol Density | `aerosol_density` | float | 1 | knob only | Aerosol |
| Ozone Density | `ozone_density` | float | 1 | knob only | Ozone |
| Turbidity | `turbidity` | float | 2.2 | knob only | Turbidity |
| Ground Albedo | `ground_albedo` | float | 0.3 | knob only | Ground Albedo |

## Sun

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Sun Disc | `sun_disc` | boolean | on | knob only | Sun Disc |
| Sun Size | `sun_size` | float | 0.009512 | knob only | Sun Size |
| Sun Intensity | `sun_intensity` | float | 1 | knob only | Sun Intensity |
| Sun Elevation | `sun_elevation` | float | 0.261799 | knob only | Sun Elevation |
| Sun Rotation | `sun_rotation` | float | 0 | knob only | Sun Rotation |
| Sun Direction | `sun_direction` | vector | 0 0 1 | knob only | Sun Direction |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Vector | `vector` | point | 0 0 0 | port | Vector |

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
