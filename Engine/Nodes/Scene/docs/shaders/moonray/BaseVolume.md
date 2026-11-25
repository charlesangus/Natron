# BaseVolume

MoonRay volume `BaseVolume`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Volume |

## Volume Baking

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Bake Resolution Mode | `bake_resolution_mode` | Int | default | knob only | Method to specify grid resolution of baked density grid.  Choices are: 		"default": For shaders that are bound to vdb volumes, use vdb resolution. 						For shaders that are bounds to mesh geometries use 100 divisions 		"divisions": Specify number of divisions. 		"voxel size": Specify voxel size. Options: default, divisions, voxel size. |
| Bake Divisions | `bake_divisions` | Int | 100 | knob only | Divide widest axis by this many divisions |
| Bake Voxel Size | `bake_voxel_size` | Float | 10 | knob only | Size of voxel in world space |

## Volume

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Surface Opacity Threshold | `surface_opacity_threshold` | Float | 0.5 | knob only | Accumulated opacity that's considered the 'surface' for computing surface position and Z |
| Emission Intensity | `emission_intensity` | Float | 1 | port | The rate at which a volume emits light at a given point.  The product of emission color and intensity is the emission coefficient. |
| Emission Color | `emission_color` | Rgb | 0 0 0 | port | A color multiplier for the emission.  The product of emission color and intensity is the emission coefficient |

## Scattering Properties

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Diffuse Color | `diffuse_color` | Rgb | 1 1 1 | port | Reflectance color of the volume. Technically this is called scattering albedo, which is the scattering coefficient divided by the extinction coefficient. |
| Use Diffuse Ramp | `use_diffuse_ramp` | Bool | off | knob only | Use a ramp to define different diffuse colors depending on the depth of the volume. |
| Diffuse Colors | `diffuse_colors` | RgbVector | 2 rows | knob only |  |
| Diffuse Distances | `diffuse_distances` | FloatVector | 2 rows | knob only |  |
| Diffuse Interpolations | `diffuse_interpolations` | IntVector | 2 rows | knob only |  |
| Diffuse Min Depth | `diffuse_min_depth` | Float | 1 | can be a port | Represents the minimum ray depth, or the shortest visible distance a ray has to travel through the volume. This sets the lower bound for the ramp.  |
| Diffuse Max Depth | `diffuse_max_depth` | Float | 2 | can be a port | Represents the maximum ray depth, or the longest visible distance a ray has to travel through the volume. This sets the upper bound for the ramp.  |
| Anisotropy | `anisotropy` | Float | 0 | port | Value in the interval [-1,1] that defines how foward (1) or backward (-1) scattering the volume is.  A value of 0.0 indicates an isotropic volume. |

## Attenuation Properties

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Attenuation Color | `attenuation_color` | Rgb | 1 1 1 | port | A color to tint (multiply to) the attenuation. Technically the product of attenuation color and intensity is the attenuation (extinction) coefficient.(Note the inverse behavior of color with this parameter.) |
| Attenuation Intensity | `attenuation_intensity` | Float | 1 | port | The rate at which the light traversing a volume is attenuated. The attenuation (extinction) coefficient is the product of attenuation_color, attenuation_intensity, and attenuation_factor |
| Attenuation Factor | `attenuation_factor` | Float | 1 | port | Identical in behavior to attenuation_intensity but provided as a second means  to control attenuation, intended for use during lighting as a per-shot or  per-sequence adjustment. |
| Match Diffuse | `match_diffuse` | Bool | off | knob only | Use the same color(s) for attenuation that is/are being used for diffuse. |
| Invert Attenuation Color | `invert_attenuation_color` | Bool | off | knob only | Invert the input attenuation color(s). |
| Use Attenuation Ramp | `use_attenuation_ramp` | Bool | off | knob only | Use a ramp to define different attenuation colors depending on the depth of the volume. |
| Attenuation Colors | `attenuation_colors` | RgbVector | 2 rows | knob only |  |
| Attenuation Distances | `attenuation_distances` | FloatVector | 2 rows | knob only |  |
| Attenuation Interpolations | `attenuation_interpolations` | IntVector | 2 rows | knob only |  |
| Attenuation Min Depth | `attenuation_min_depth` | Float | 1 | can be a port | Represents the minimum ray depth, or the shortest visible distance a ray has to travel through the volume. This sets the lower bound for the ramp.  |
| Attenuation Max Depth | `attenuation_max_depth` | Float | 2 | can be a port | Represents the maximum ray depth, or the longest visible distance a ray has to travel through the volume. This sets the upper bound for the ramp.  |

## Density Properties

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Densities | `densities` | FloatVector | 2 rows | knob only |  |
| Density Distances | `density_distances` | FloatVector | 2 rows | knob only |  |
| Density Interpolations | `density_interpolations` | IntVector | 2 rows | knob only |  |
| Density Min Depth | `density_min_depth` | Float | 1 | can be a port | Represents the minimum ray depth, or the shortest visible distance a ray has to travel through the volume. This sets the lower bound for the ramp.  |
| Density Max Depth | `density_max_depth` | Float | 2 | can be a port | Represents the maximum ray depth, or the longest visible distance a ray has to travel through the volume. This sets the upper bound for the ramp.  |

## Other

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Label | `label` | String |  | knob only | label used in light aovs |
