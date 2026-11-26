# Hair Material V 3

MoonRay material `HairMaterial_v3`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | DwaBaseHairLayerable |

## Common

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Presence | `presence` | Float | 1 | port | controls the visibility of this object. Useful for fading an object in/out, or to specify a cut-out mask on thin single-sided geometry (eg. a complex leaf texture on a simple card). |
| Casts Caustics | `casts_caustics` | Bool | off | knob only | allows continuation of caustic light paths. |

## Diffuse

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Diffuse Lightset | `diffuse_lightset` | SceneObject* |  | port only | lightset to use for diffuse lobes |

## Specular

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Specular Lightset | `specular_lightset` | SceneObject* |  | port only | lightset to use for specular lobes |

## Fresnel

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Refractive Index | `refractive_index` | Float | 1.45 | knob only | keep this value between [1.3,2.0] for realistic behavior (human hair is around 1.55) |
| Fresnel Type | `fresnel_type` | Int | dielectric cylinder | knob only | Options: simple longitudinal, dielectric cylinder, layered cuticles. |
| Cuticle Layer Thickness | `cuticle_layer_thickness` | Float | 0.1 | port |  |

## Primary Specular

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Show Primary Specular | `show_primary_specular` | Bool | on | knob only | show primary specular |
| Primary Specular Offset | `primary_specular_offset` | Float | -3 | can be a port | offset specular highlight along hair direction (in degrees) [-10,+10], around -3 for human hair |
| Primary Specular Roughness | `primary_specular_roughness` | Float | 0.5 | port | roughness of the primary specular highlight, also sets the transmission roughness to 0.5x and secondary specular roughness to 2x by default unless independent roughnesses are being used for both |
| Primary Specular Tint | `primary_specular_tint` | Rgb | 1 1 1 | port | tint the primary specular highlight with this color (leave white for physical behavior) |

## Secondary Specular

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Show Secondary Specular | `show_secondary_specular` | Bool | on | knob only | show secondary specular |
| Secondary Specular Offset | `secondary_specular_offset` | Float | -4.5 | can be a port | offset secondary specular highlight along hair direction (in degrees) [-10,+10], around -4.5 for human hair |
| Use Independent Secondary Specular Roughness | `use_independent_secondary_specular_roughness` | Bool | off | knob only | when disabled, uses a physically correct value for secondary specular roughness which is linked to the primary specular roughness |
| Independent Secondary Specular Roughness | `independent_secondary_specular_roughness` | Float | 0.4 | can be a port | secondary specular roughness |
| Secondary Specular Tint | `secondary_specular_tint` | Rgb | 1 1 1 | port | tint secondary specular with this color (leave white for physical behavior) |
| Show Hair Glint | `show_hair_glint` | Bool | off | knob only | show hair glint |
| Glint Roughness | `glint_roughness` | Float | 0.5 | port | secondary specular glint roughness |
| Glint Eccentricity | `glint_eccentricity` | Float | 0.85 | can be a port | secondary specular glint eccentricity [0.85, 1], values that deviate from 1 make the hair fiber more elliptical and more glinty |
| Glint Min Twists | `glint_min_twists` | Float | 1.5 | can be a port | the minimum number of twists along the hair's length. More twists means more glints. Each hair strand will be randomly assigned a twist amount between [min twists, max twists] |
| Glint Max Twists | `glint_max_twists` | Float | 2.5 | can be a port | the maximum number of twists along the hair's length. More twists means more glints. Each hair strand will be randomly assigned a twist amount between [min twists, max twists] |
| Glint Saturation | `glint_saturation` | Float | 0.5 | can be a port | secondary specular glint saturation |

## Transmission

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Show Transmission | `show_transmission` | Bool | on | knob only | show tranmission specular |
| Transmission Offset | `transmission_offset` | Float | -1.5 | can be a port | offset transmission highlight along hair direction (in degrees) [-10,+10], around -1.5 for human hair |
| Use Independent Transmission Roughness | `use_independent_transmission_roughness` | Bool | off | knob only | when disabled, uses a physically correct value for Transmission roughness which is linked to the primary specular roughness |
| Independent Transmission Roughness | `independent_transmission_roughness` | Float | 0.1 | port | transmission roughness |
| Transmission Azimuthal Roughness | `transmission_azimuthal_roughness` | Float | 1 | can be a port | higher values create a softer look |
| Transmission Tint | `transmission_tint` | Rgb | 1 1 1 | port | tint transmission with this color (leave white for physical behavior) |
| Direct Transmission Saturation | `direct_transmission_saturation` | Float | 1 | can be a port | (Non-Physical, Advanced) saturate/desaturate direct transmission highlights. |

## Advanced

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Use Optimized Sampling | `use_optimized_sampling` | Bool | on | knob only | optimized sampling of all the hair lobes, results in 2x-4x speedup on average (disables individual hair lobe AOVs). When false, the look may slightly change if using biased techniques like roughness/sample clamping.  |
| Show Multiple Scattering | `show_multiple_scattering` | Bool | on | knob only | show the amount of light scattered after TRT bounce, good for blonde/white hair |

## Emission

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Show Emission | `show_emission` | Bool | off | knob only | enables/disable emission |
| Emission | `emission` | Rgb | 1 1 1 | port | the energy emitted from this material |

## Other

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Extra AOVs | `extra_aovs` | SceneObject* |  | port only | Bind this attribute to a 'ListMap' that contains references to ExtraAovMaps that specify additional outputs that can be assigned to a RenderOutput "light aov" result |
| Label | `label` | String |  | knob only | label used in material and light aovs |
| Priority | `priority` | Int | 0 | knob only | The material's place in an order of precedence for overlapping dielectrics. A value of 0 means the priority should be ignored. Materials with lower numbers (higher priority) "override" materials with higher numbers (lower priority).  To enable automatic removal of self-overlapping geometry, a non-zero priority must be set on the geometry's material. |
| Record Reflected Cryptomatte | `record_reflected_cryptomatte` | Bool | off | knob only | Indicates whether the next reflected surface should appear in the reflected cryptomatte layers |
| Record Refracted Cryptomatte | `record_refracted_cryptomatte` | Bool | off | knob only | Indicates whether the next refracted surface should appear in the refracted cryptomatte layers |
| Hair Color | `hair_color` | Rgb | 1 1 1 | port |  |
