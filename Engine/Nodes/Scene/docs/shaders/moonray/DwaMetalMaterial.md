# DwaMetalMaterial

MoonRay material `DwaMetalMaterial`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | DwaBaseLayerable |

## Fuzz

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Show Fuzz | `show_fuzz` | Bool | off | knob only | Enables/disables fuzz lobe |
| Fuzz | `fuzz` | Float | 1 | port | fuzz mask |
| Fuzz Roughness | `fuzz_roughness` | Float | 0.25 | can be a port | Lower values result in glancing angle highlights while higher values result in a broad, uniform coverage |
| Fuzz Albedo | `fuzz_albedo` | Rgb | 1 1 1 | can be a port | Color of the fuzz highlights. |
| Use Absorbing Fuzz Fibers | `use_absorbing_fuzz_fibers` | Bool | off | knob only | Specify whether dark fuzz fibers absorb energy or transmit it to the layers below. |
| Fuzz Normal | `fuzz_normal` | SceneObject* |  | port only | specifies an independent shading normal (normal map) for the fuzz lobe |
| Fuzz Normal Dial | `fuzz_normal_dial` | Float | 1 | can be a port | controls the amount of infuence of the alternate fuzz normal |

## Clearcoat

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Show Clearcoat | `show_clearcoat` | Bool | off | knob only | enables/disables clearcoat |
| Clearcoat | `clearcoat` | Float | 1 | port | enables/disables clearcoat (binary 0\|1 for plausibility) |
| Clearcoat Model | `clearcoat_model` | Int | GGX | knob only | sets the normal distribution function for clearcoat.  GGX is currently isotropic only Options: Beckmann, GGX. |
| Clearcoat Refractive Index | `clearcoat_refractive_index` | Float | 1.5 | knob only | defines the Fresnel behavior |
| Clearcoat Roughness | `clearcoat_roughness` | Float | 0.1 | port | the roughness of the clearcoat lobe |
| Clearcoat Thickness | `clearcoat_thickness` | Float | 0 | can be a port | the thickness of the virtual clearcoat layer. Values > 0 enable absorption |
| Clearcoat Attenuation Color | `clearcoat_attenuation_color` | Rgb | 0.5 0.5 0.5 | can be a port | the attenuation color of the clearcoat when 'cleacoat thickness' > 0 |
| Clearcoat Bending | `clearcoat_bending` | Bool | on | knob only | (advanced, recommended ON) bends rays based on the clearcoat-refractive-index before evaluating the lobes under clearcoat |
| Use Independent Clearcoat Normal | `use_independent_clearcoat_normal` | Bool | off | knob only | specifies whether the clearcoat lobe should use an independent normal |
| Independent Clearcoat Normal | `independent_clearcoat_normal` | SceneObject* |  | port only | specifies an independent shading normal (normal map) for the clearcoat lobe |
| Clearcoat Normal Dial | `clearcoat_normal_dial` | Float | 1 | can be a port | controls the amount of infuence of the alternate clearcoat normal |

## Glitter

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Show Glitter | `show_glitter` | Bool | off | knob only | Enables/disables glitter lobes |
| Glitter | `glitter` | Float | 1 | port | enables/disables glitter model (binary 0\|1 for plausibility) |
| Glitter Seed | `glitter_seed` | Int | 0 | knob only | The seed for the glitter random number generator |
| Glitter Space | `glitter_space` | Int | reference | knob only | The space to calculate the worley noise in, defaults to reference space Options: object, reference. |
| Glitter Density | `glitter_density` | Float | 1 | can be a port | controls the number of flakes per unit length; larger density packs more flakes into same space |
| Glitter Randomness | `glitter_randomness` | Float | 0.5 | knob only | randomness of flake orientation |
| Glitter Layering Mode | `glitter_layering_mode` | Int | physical | knob only | layering mode for glitter on top of the under material. physical: conserves energy and glitter attenuates under material, additive: breaks energy conservation but glitter is never darker than the under material (eg. use case: snow) Options: physical, additive. |
| Glitter Style A Frequency | `glitter_style_A_frequency` | Float | 1 | can be a port | 0 implies none of this style, 1 implies all the flakes will get this style |
| Glitter Size A | `glitter_size_A` | Float | 1 | can be a port | size of the flakes.  Apparent flake size may vary based on how much the flake spheres intersect the surface |
| Glitter Roughness A | `glitter_roughness_A` | Float | 0.14 | knob only | specular roughness of individual flakes (0 makes flakes mirror-like) |
| Glitter Color A | `glitter_color_A` | Rgb | 1 1 1 | can be a port | base flake color (use physical metallic color values) |
| Glitter Texture A | `glitter_texture_A` | String |  | knob only | filename that points to a texture .exr or .tx file (must be mip-mapped and tiled with maketx). |
| Glitter Style B Frequency | `glitter_style_B_frequency` | Float | 0 | can be a port | 0 implies none of this style, 1 implies all the flakes will get this style |
| Glitter Size B | `glitter_size_B` | Float | 1 | can be a port | size of the flakes.  Apparent flake size may vary based on how much the flake spheres intersect the surface |
| Glitter Roughness B | `glitter_roughness_B` | Float | 0.14 | knob only | specular roughness of individual flakes (0 makes flakes mirror-like) |
| Glitter Color B | `glitter_color_B` | Rgb | 1 1 1 | can be a port | base flake color (use physical metallic color values) |
| Glitter Texture B | `glitter_texture_B` | String |  | knob only | filename that points to a texture .exr or .tx file (must be mip-mapped and tiled with maketx). |
| Glitter Texture Orientation Randomness | `glitter_texture_orientation_randomness` | Float | 0.15 | knob only | randomly orient each texture |
| Glitter Color Hue Variation | `glitter_color_hue_variation` | Float | 0 | can be a port | introduce hue variation in flake color centered at the base flake color's hue on the hue wheel |
| Glitter Color Saturation Variation | `glitter_color_saturation_variation` | Float | 0 | can be a port | introduce saturation variation in flake color centered at the base flake color's saturation |
| Glitter Color Value Variation | `glitter_color_value_variation` | Float | 0 | can be a port | introduce value variation in flake color centered at the base flake color's value |
| Glitter Jitter | `glitter_jitter` | Float | 1 | can be a port | Controls how much the flakes are randomly offset from a regular grid |
| Glitter Compensate Reference Space Deformation | `glitter_compensate_reference_space_deformation` | Bool | on | knob only | (In ReferenceSpace) Compensates for stretch/compression/shear in glitter shapes resulting from animation etc |
| Glitter Approximate For Secondary Rays | `glitter_approximate_for_secondary_rays` | Bool | on | knob only | use an approximation to shade glitter for non-mirror secondary rays |
| Glitter LOD Quality | `glitter_LOD_quality` | Float | 0.5 | knob only | controls quality of glitter at distances where individual flakes cannot be perceived; at lower values, approximation kicks in earlier |
| Glitter Debug Mode | `glitter_debug_mode` | Int | off | knob only | developer debug visualization modes Options: off, blend, color, averageColor, footprintArea, radius. |

## Specular

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Show Specular | `show_specular` | Bool | on | knob only | enables/disables specular reflections |
| Specular Model | `specular_model` | Int | GGX | knob only | sets the normal distribution function for specular.  GGX is currently isotropic only Options: Beckmann, GGX. |
| Metallic Color | `metallic_color` | Rgb | 1 1 1 | port | the overall reflection color, defines Fresnel behavior |
| Metallic Edge Color | `metallic_edge_color` | Rgb | 1 1 1 | port | the reflection color at grazing angles, defines Fresnel behavior |
| Roughness | `roughness` | Float | 0.5 | port | the roughness of the surface (currently only affects reflection) |
| Anisotropy | `anisotropy` | Float | 0 | port | controls the shape of the primary reflection |
| Shading Tangent | `shading_tangent` | Vec2f | 1 0 | can be a port | controls the orientation of anistropy |

## Iridescence

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Iridescence | `iridescence` | Float | 0 | port | controls the strength of the iridescence effect |
| Iridescence Apply To | `iridescence_apply_to` | Int | primary specular | knob only | Apply iridescence to primary specular lobe or clearcoat/moisture lobe Options: primary specular, clearcoat/moisture specular. |
| Iridescence Color Control | `iridescence_color_control` | Int | use hue interpolation | knob only | use hue interpolation: automatically cycles through hue wheel, use ramp: user specified color ramp Options: use hue interpolation, use ramp. |
| Iridescence Primary Color | `iridescence_primary_color` | Rgb | 1 0 0 | can be a port | First color to interpolate from around the hue wheel |
| Iridescence Secondary Color | `iridescence_secondary_color` | Rgb | 1 0 0 | can be a port | Second color to interpolate to around the hue wheel |
| Iridescence Flip Hue Direction | `iridescence_flip_hue_direction` | Bool | off | knob only | flip interpolation around the hue wheel to counter-clockwise direction |
| Iridescence Ramp Interpolation Mode | `iridescence_ramp_interpolation_mode` | Int | RGB | knob only | RGB: lerp in RGB space which matches UI preview but can lose saturation, HSV: lerp in HSV space which preserves saturation Options: RGB, HSV. |
| Iridescence Positions | `iridescence_positions` | FloatVector | 7 rows | knob only | ramp positions |
| Iridescence Colors | `iridescence_colors` | RgbVector | 7 rows | knob only | List of colors on the ramp |
| Iridescence Interpolations | `iridescence_interpolations` | IntVector | 7 rows | knob only | None: 0 \| Linear: 1 \| Exponential Up: 2 \| Exponential Down: 3 \| 			Smooth: 4 \| Catmull Rom: 5 \| Monotone Cubic: 6 |
| Iridescence Thickness | `iridescence_thickness` | Float | 1 | can be a port | Controls how much the color spectrum is repeated |
| Iridescence Exponent | `iridescence_exponent` | Float | 1 | can be a port | Tightens or broadens the distribution of colors |
| Iridescence At 0 Incidence | `iridescence_at_0_incidence` | Float | 1 | can be a port | Iridescence effect at 0 degree viewing angle |
| Iridescence At 90 Incidence | `iridescence_at_90_incidence` | Float | 1 | can be a port | Iridescence effect at 90 degree viewing angle |

## Emission

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Show Emission | `show_emission` | Bool | off | knob only | enables/disable emission |
| Emission | `emission` | Rgb | 1 1 1 | port | the energy emitted from this material |

## Normal

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Input Normal | `input_normal` | SceneObject* |  | port only | specifies an alternate shading normal in the tangent frame (normal map) |
| Input Normal Dial | `input_normal_dial` | Float | 1 | can be a port | controls the amount of influence of the alternate normal |

## Normal Anti-aliasing

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Normal AA Strategy | `normal_AA_strategy` | Int | none | knob only | Normal map anti-aliasing strategy to use - 'none' uses regular mip-mapping, 'toksvig' increases specular roughness corresponding to the geometric details filtered out because of mip-mapping. Options: none, toksvig. |
| Normal AA Dial | `normal_AA_dial` | Float | 1 | knob only | Controls the amount roughness compensation from the normal map AA strategy. |

## Common

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Presence | `presence` | Float | 1 | port | controls the visibility of this object. Useful for fading an object in/out, or to specify a cut-out mask on thin single-sided geometry (eg. a complex leaf texture on a simple card). |
| Thin Geometry | `thin_geometry` | Bool | off | knob only | enables proper shading of infinitely thin geometry such as paper or leaves. |
| Casts Caustics | `casts_caustics` | Bool | off | knob only | allows continuation of caustic light paths. |

## Advanced

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Specular | `specular` | Float | 1 | port | enables/disables specular reflections (binary 0\|1 for plausibility) |

## Other

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Extra AOVs | `extra_aovs` | SceneObject* |  | port only | Bind this attribute to a 'ListMap' that contains references to ExtraAovMaps that specify additional outputs that can be assigned to a RenderOutput "light aov" result |
| Label | `label` | String |  | knob only | label used in material and light aovs |
| Priority | `priority` | Int | 0 | knob only | The material's place in an order of precedence for overlapping dielectrics. A value of 0 means the priority should be ignored. Materials with lower numbers (higher priority) "override" materials with higher numbers (lower priority).  To enable automatic removal of self-overlapping geometry, a non-zero priority must be set on the geometry's material. |
| Record Reflected Cryptomatte | `record_reflected_cryptomatte` | Bool | off | knob only | Indicates whether the next reflected surface should appear in the reflected cryptomatte layers |
| Record Refracted Cryptomatte | `record_refracted_cryptomatte` | Bool | off | knob only | Indicates whether the next refracted surface should appear in the refracted cryptomatte layers |
