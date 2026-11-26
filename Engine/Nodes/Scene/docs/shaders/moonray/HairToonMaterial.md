# Hair Toon Material

MoonRay material `HairToonMaterial`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | DwaBaseHairLayerable |

## Diffuse

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Show Hair Diffuse | `show_hair_diffuse` | Bool | on | knob only | Show the hair diffuse lobe |
| Hair Diffuse | `hair_diffuse` | Float | 1 | port | Amount of hair diffuse |
| Hair Color | `hair_color` | Rgb | 1 1 1 | port |  |
| Diffuse Lightset | `diffuse_lightset` | SceneObject* |  | port only | lightset to use for diffuse lobes |

## Specular 1

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Specular 1 Show | `specular_1_show` | Bool | on | knob only | Show first toon specular lobe |
| Specular 1 Model | `specular_1_model` | Int | Toon_Surface | knob only | sets the normal distribution function for specular Options: Toon_Surface, Toon_Hair. |
| Specular 1 Intensity | `specular_1_intensity` | Float | 1 | port | The overall intensity of the specular response |
| Specular 1 Tint | `specular_1_tint` | Rgb | 1 1 1 | port |  |
| Specular 1 Roughness | `specular_1_roughness` | Float | 0.9 | port | The roughness of the toon specular.   Smaller values produce tighter highlights |
| Specular 1 Stretch U | `specular_1_stretch_u` | Float | 0 | can be a port | Amount to stretch or compress the specular in the u direction  |
| Specular 1 Stretch V | `specular_1_stretch_v` | Float | 0 | can be a port | Amount to stretch or compress the specular in the v direction  |
| Specular 1 Use Input Vectors For Stretch | `specular_1_use_input_vectors_for_stretch` | Bool | off | knob only | when checked, use input_U and V. otherwise use geometry dPds/t |
| Specular 1 Input U | `specular_1_input_U` | Vec3f | 0 0 0 | can be a port | input U / tangent for specular stretch |
| Specular 1 Input V | `specular_1_input_V` | Vec3f | 0 0 0 | can be a port | input V / bitangent for specular stretch |
| Specular 1 Ramp | `specular_1_ramp` | ramp_float | 4 points, constant | knob only | List of colors on the ramp |
| Specular 1 Mult 0 | `specular_1_mult0` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 1 Mult 1 | `specular_1_mult1` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 1 Mult 2 | `specular_1_mult2` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 1 Mult 3 | `specular_1_mult3` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 1 Mult 4 | `specular_1_mult4` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 1 Mult 5 | `specular_1_mult5` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 1 Mult 6 | `specular_1_mult6` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 1 Mult 7 | `specular_1_mult7` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 1 Mult 8 | `specular_1_mult8` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 1 Mult 9 | `specular_1_mult9` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 1 Offset 0 | `specular_1_offset0` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 1 Offset 1 | `specular_1_offset1` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 1 Offset 2 | `specular_1_offset2` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 1 Offset 3 | `specular_1_offset3` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 1 Offset 4 | `specular_1_offset4` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 1 Offset 5 | `specular_1_offset5` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 1 Offset 6 | `specular_1_offset6` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 1 Offset 7 | `specular_1_offset7` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 1 Offset 8 | `specular_1_offset8` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 1 Offset 9 | `specular_1_offset9` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 1 Enable Input Normal | `specular_1_enable_input_normal` | Bool | off | knob only | enables sampling the normal map for toon specular 1 |
| Specular 1 Input Normal | `specular_1_input_normal` | SceneObject* |  | port only | specifies an alternate shading normal for toon specular 1 |
| Specular 1 Input Normal Dial | `specular_1_input_normal_dial` | Float | 1 | can be a port | controls influence of input normal versus hair normal for toon specular 1 |
| Specular 1 Enable Indirect Reflections | `specular_1_enable_indirect_reflections` | Bool | off | knob only | enables indirect GGX reflections for toon specular model |
| Specular 1 Indirect Reflections Intensity | `specular_1_indirect_reflections_intensity` | Float | 1 | can be a port | the intensity for the indirect reflections of the toon specular model |
| Specular 1 Indirect Reflections Roughness | `specular_1_indirect_reflections_roughness` | Float | 0.5 | can be a port | the roughness for the indirect reflections of the toon specular model |

## Specular

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Specular 1 Fresnel Blend | `specular_1_fresnel_blend` | Float | 1 | can be a port | The amount of fresnel to use for the toon specular response |
| Specular 1 Ramp Input Scale | `specular_1_ramp_input_scale` | Float | 1 | can be a port | Scales the input value x to the ramp lookup y, where x is based on the angle between the sample direction and normal.  This has the effect of squashing or stretching the ramp point positions towards/away from 0. |
| Specular 2 Fresnel Blend | `specular_2_fresnel_blend` | Float | 1 | can be a port | The amount of fresnel to use for the toon specular response |
| Specular 2 Ramp Input Scale | `specular_2_ramp_input_scale` | Float | 1 | can be a port | Scales the input value x to the ramp lookup y, where x is based on the angle between the sample direction and normal.  This has the effect of squashing or stretching the ramp point positions towards/away from 0. |
| Specular 3 Fresnel Blend | `specular_3_fresnel_blend` | Float | 1 | can be a port | The amount of fresnel to use for the toon specular response |
| Specular 3 Ramp Input Scale | `specular_3_ramp_input_scale` | Float | 1 | can be a port | Scales the input value x to the ramp lookup y, where x is based on the angle between the sample direction and normal.  This has the effect of squashing or stretching the ramp point positions towards/away from 0. |
| Specular Lightset | `specular_lightset` | SceneObject* |  | port only | lightset to use for specular lobes |

## Specular 2

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Specular 2 Show | `specular_2_show` | Bool | off | knob only | Show second toon specular lobe |
| Specular 2 Model | `specular_2_model` | Int | Toon_Surface | knob only | sets the normal distribution function for specular Options: Toon_Surface, Toon_Hair. |
| Specular 2 Intensity | `specular_2_intensity` | Float | 1 | port | The overall intensity of the specular response |
| Specular 2 Tint | `specular_2_tint` | Rgb | 1 1 1 | port |  |
| Specular 2 Roughness | `specular_2_roughness` | Float | 0.9 | can be a port | The roughness of the toon specular.   Smaller values produce tighter highlights |
| Specular 2 Stretch U | `specular_2_stretch_u` | Float | 0 | can be a port | Amount to stretch or compress the specular in the u direction  |
| Specular 2 Stretch V | `specular_2_stretch_v` | Float | 0 | can be a port | Amount to stretch or compress the specular in the v direction  |
| Specular 2 Use Input Vectors For Stretch | `specular_2_use_input_vectors_for_stretch` | Bool | off | knob only | when checked, use input_U and V. otherwise use geometry dPds/t |
| Specular 2 Input U | `specular_2_input_U` | Vec3f | 0 0 0 | can be a port | input U / tangent for specular stretch |
| Specular 2 Input V | `specular_2_input_V` | Vec3f | 0 0 0 | can be a port | input V / bitangent for specular stretch |
| Specular 2 Ramp | `specular_2_ramp` | ramp_float | 4 points, constant | knob only | List of colors on the ramp |
| Specular 2 Mult 0 | `specular_2_mult0` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 2 Mult 1 | `specular_2_mult1` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 2 Mult 2 | `specular_2_mult2` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 2 Mult 3 | `specular_2_mult3` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 2 Mult 4 | `specular_2_mult4` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 2 Mult 5 | `specular_2_mult5` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 2 Mult 6 | `specular_2_mult6` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 2 Mult 7 | `specular_2_mult7` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 2 Mult 8 | `specular_2_mult8` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 2 Mult 9 | `specular_2_mult9` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 2 Offset 0 | `specular_2_offset0` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 2 Offset 1 | `specular_2_offset1` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 2 Offset 2 | `specular_2_offset2` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 2 Offset 3 | `specular_2_offset3` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 2 Offset 4 | `specular_2_offset4` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 2 Offset 5 | `specular_2_offset5` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 2 Offset 6 | `specular_2_offset6` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 2 Offset 7 | `specular_2_offset7` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 2 Offset 8 | `specular_2_offset8` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 2 Offset 9 | `specular_2_offset9` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 2 Enable Input Normal | `specular_2_enable_input_normal` | Bool | off | knob only | enables sampling the normal map for toon specular 2 |
| Specular 2 Input Normal | `specular_2_input_normal` | SceneObject* |  | port only | specifies an alternate shading normal for toon specular 2 |
| Specular 2 Input Normal Dial | `specular_2_input_normal_dial` | Float | 1 | can be a port | controls influence of input normal versus hair normal for toon specular 2 |
| Specular 2 Enable Indirect Reflections | `specular_2_enable_indirect_reflections` | Bool | off | knob only | enables indirect GGX reflections for toon specular model |
| Specular 2 Indirect Reflections Intensity | `specular_2_indirect_reflections_intensity` | Float | 1 | can be a port | the intensity for the indirect reflections of the toon specular model |
| Specular 2 Indirect Reflections Roughness | `specular_2_indirect_reflections_roughness` | Float | 0.5 | can be a port | the roughness for the indirect reflections of the toon specular model |

## Specular 3

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Specular 3 Show | `specular_3_show` | Bool | off | knob only | Show third toon specular lobe |
| Specular 3 Model | `specular_3_model` | Int | Toon_Surface | knob only | sets the normal distribution function for specular Options: Toon_Surface, Toon_Hair. |
| Specular 3 Intensity | `specular_3_intensity` | Float | 1 | can be a port | The overall intensity of the specular response |
| Specular 3 Tint | `specular_3_tint` | Rgb | 1 1 1 | can be a port |  |
| Specular 3 Roughness | `specular_3_roughness` | Float | 0.9 | can be a port | The roughness of the toon specular.   Smaller values produce tighter highlights |
| Specular 3 Stretch U | `specular_3_stretch_u` | Float | 0 | can be a port | Amount to stretch or compress the specular in the u direction  |
| Specular 3 Stretch V | `specular_3_stretch_v` | Float | 0 | can be a port | Amount to stretch or compress the specular in the v direction  |
| Specular 3 Use Input Vectors For Stretch | `specular_3_use_input_vectors_for_stretch` | Bool | off | knob only | when checked, use input_U and V. otherwise use geometry dPds/t |
| Specular 3 Input U | `specular_3_input_U` | Vec3f | 0 0 0 | can be a port | input U / tangent for specular stretch |
| Specular 3 Input V | `specular_3_input_V` | Vec3f | 0 0 0 | can be a port | input V / bitangent for specular stretch |
| Specular 3 Ramp | `specular_3_ramp` | ramp_float | 4 points, constant | knob only | List of colors on the ramp |
| Specular 3 Mult 0 | `specular_3_mult0` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 3 Mult 1 | `specular_3_mult1` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 3 Mult 2 | `specular_3_mult2` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 3 Mult 3 | `specular_3_mult3` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 3 Mult 4 | `specular_3_mult4` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 3 Mult 5 | `specular_3_mult5` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 3 Mult 6 | `specular_3_mult6` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 3 Mult 7 | `specular_3_mult7` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 3 Mult 8 | `specular_3_mult8` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 3 Mult 9 | `specular_3_mult9` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Specular 3 Offset 0 | `specular_3_offset0` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 3 Offset 1 | `specular_3_offset1` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 3 Offset 2 | `specular_3_offset2` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 3 Offset 3 | `specular_3_offset3` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 3 Offset 4 | `specular_3_offset4` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 3 Offset 5 | `specular_3_offset5` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 3 Offset 6 | `specular_3_offset6` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 3 Offset 7 | `specular_3_offset7` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 3 Offset 8 | `specular_3_offset8` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 3 Offset 9 | `specular_3_offset9` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Specular 3 Enable Input Normal | `specular_3_enable_input_normal` | Bool | off | knob only | enables sampling the normal map for toon specular 3 |
| Specular 3 Input Normal | `specular_3_input_normal` | SceneObject* |  | port only | specifies an alternate shading normal for toon specular 3 |
| Specular 3 Input Normal Dial | `specular_3_input_normal_dial` | Float | 1 | can be a port | controls influence of input normal versus hair normal for toon specular 3 |
| Specular 3 Enable Indirect Reflections | `specular_3_enable_indirect_reflections` | Bool | off | knob only | enables indirect GGX reflections for toon specular model |
| Specular 3 Indirect Reflections Intensity | `specular_3_indirect_reflections_intensity` | Float | 1 | can be a port | the intensity for the indirect reflections of the toon specular model |
| Specular 3 Indirect Reflections Roughness | `specular_3_indirect_reflections_roughness` | Float | 0.5 | can be a port | the roughness for the indirect reflections of the toon specular model |

## Advanced

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Use Independent Front And Back Hair Color | `use_independent_front_and_back_hair_color` | Bool | off | knob only | (advanced) use a separate hair color for front and back |
| Front Hair Color | `front_hair_color` | Rgb | 1 1 1 | port | (advanced only) hair color used for front-lit hair (backward reflectance) |
| Back Hair Color | `back_hair_color` | Rgb | 1 1 1 | port | (advanced only) hair color used for back-lit hair (transmission/forward reflectance) |
| SSS Trace Set | `sss_trace_set` | SceneObject* |  | port only | Set of geometries that contribute neighboring subsurface points. By default, only the geometry associated with this material contributes to subsurface. If you want adjacent geometry with different material to contribute as well, specify all those parts here. |

## Subsurface

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Subsurface Blend | `subsurface_blend` | Float | 1 | can be a port | 0 is fully hair diffuse, 1 is fully SSS. No effect if scattering radius is 0. |
| BSSRDF | `bssrdf` | Int | normalized diffusion | knob only | 0 for NormalizedDiffuse, 1 for Dipole. Random walk unsupported for hair. Options: normalized diffusion, dipole. |
| Scattering Color | `scattering_color` | Rgb | 1 1 1 | port | the subsurface scattering 'falloff' color |
| Scattering Radius | `scattering_radius` | Float | 0 | can be a port | the distance the light scatters beneath the surface.  When 0 surface diffuse is used |
| Enable SSS Input Normal | `enable_sss_input_normal` | Bool | off | knob only | enables sampling the normal map for sss samples. More accurate but potentially expensive |
| Input Normal | `input_normal` | SceneObject* |  | port only | specifies an alternate shading normal (only for SSS lobe) |
| Input Normal Dial | `input_normal_dial` | Float | 1 | can be a port | controls influence of input normal versus hair normal for SSS |

## Common

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Presence | `presence` | Float | 1 | port | controls the visibility of this object. Useful for fading an object in/out, or to specify a cut-out mask on thin single-sided geometry (eg. a complex leaf texture on a simple card). |

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
