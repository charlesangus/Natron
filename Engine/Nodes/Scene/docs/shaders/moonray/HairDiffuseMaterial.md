# Hair Diffuse Material

MoonRay material `HairDiffuseMaterial`, for the moonray target.

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
| Subsurface Blend | `subsurface_blend` | Float | 1 | port | 0 is fully hair diffuse, 1 is fully SSS. No effect if scattering radius is 0. |
| BSSRDF | `bssrdf` | Int | normalized diffusion | knob only | 0 for NormalizedDiffuse, 1 for Dipole. Random walk unsupported for hair. Options: normalized diffusion, dipole. |
| Scattering Color | `scattering_color` | Rgb | 1 1 1 | port | the subsurface scattering 'falloff' color |
| Scattering Radius | `scattering_radius` | Float | 0 | port | the distance the light scatters beneath the surface.  When 0 surface diffuse is used |
| Enable SSS Input Normal | `enable_sss_input_normal` | Bool | off | knob only | enables sampling the normal map for sss samples. More accurate but potentially expensive |
| Input Normal | `input_normal` | SceneObject* |  | port only | specifies an alternate shading normal (only for SSS lobe) |
| Input Normal Dial | `input_normal_dial` | Float | 1 | port | controls influence of input normal versus hair normal for SSS |

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
