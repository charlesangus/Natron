# Dwa Toon Material

MoonRay material `DwaToonMaterial`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | DwaBaseLayerable |

## Fuzz

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Show Fuzz | `show_fuzz` | Bool | off | knob only | Enables/disables fuzz lobe |
| Fuzz | `fuzz` | Float | 1 | can be a port | fuzz mask |
| Fuzz Roughness | `fuzz_roughness` | Float | 0.25 | can be a port | Lower values result in glancing angle highlights while higher values result in a broad, uniform coverage |
| Fuzz Albedo | `fuzz_albedo` | Rgb | 1 1 1 | can be a port | Color of the fuzz highlights. |
| Use Absorbing Fuzz Fibers | `use_absorbing_fuzz_fibers` | Bool | off | knob only | Specify whether dark fuzz fibers absorb energy or transmit it to the layers below. |
| Fuzz Normal | `fuzz_normal` | SceneObject* |  | port only | specifies an independent shading normal (normal map) for the fuzz lobe |
| Fuzz Normal Dial | `fuzz_normal_dial` | Float | 1 | can be a port | controls the amount of infuence of the alternate fuzz normal |

## Clearcoat

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Show Clearcoat | `show_clearcoat` | Bool | off | knob only | enables/disables clearcoat |
| Clearcoat | `clearcoat` | Float | 1 | can be a port | enables/disables clearcoat (binary 0\|1 for plausibility) |
| Clearcoat Model | `clearcoat_model` | Int | GGX | knob only | sets the normal distribution function for clearcoat.  GGX is currently isotropic only Options: Beckmann, GGX. |
| Clearcoat Refractive Index | `clearcoat_refractive_index` | Float | 1.5 | knob only | defines the Fresnel behavior |
| Clearcoat Roughness | `clearcoat_roughness` | Float | 0.1 | can be a port | the roughness of the clearcoat lobe |
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
| Glitter | `glitter` | Float | 1 | can be a port | enables/disables glitter model (binary 0\|1 for plausibility) |
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
| Specular Model | `specular_model` | Int | GGX | knob only | sets the normal distribution function for specular.  GGX is currently isotropic only Options: Beckmann, GGX, Toon. |
| Roughness | `roughness` | Float | 0.5 | port | the roughness of the surface |
| Refractive Index | `refractive_index` | Float | 1.5 | knob only | defines the Fresnel behavior (affects only refraction when model is Toon) |
| Anisotropy | `anisotropy` | Float | 0 | can be a port | controls the shape of the primary reflection |
| Shading Tangent | `shading_tangent` | Vec2f | 1 0 | can be a port | controls the orientation of anistropy |
| Metallic | `metallic` | Float | 0 | port | enables/disables metallic model (binary 0\|1 for plausibility) |
| Metallic Color | `metallic_color` | Rgb | 1 1 1 | port | the overall reflection color, defines Fresnel behavior |
| Metallic Edge Color | `metallic_edge_color` | Rgb | 1 1 1 | can be a port | the reflection color at grazing angles, defines Fresnel behavior |
| Toon Specular Intensity | `toon_specular_intensity` | Float | 1 | port | The overall intensity of the toon specular response |
| Toon Specular Fresnel Blend | `toon_specular_fresnel_blend` | Float | 1 | can be a port | The amount of fresnel to use for the toon specular response |
| Toon Specular Tint | `toon_specular_tint` | Rgb | 1 1 1 | port |  |
| Toon Specular Stretch U | `toon_specular_stretch_u` | Float | 0 | can be a port | Amount to stretch or compress the specular in the u direction  |
| Toon Specular Stretch V | `toon_specular_stretch_v` | Float | 0 | can be a port | Amount to stretch or compress the specular in the v direction  |
| Toon Specular Use Input Vectors For Stretch | `toon_specular_use_input_vectors_for_stretch` | Bool | off | knob only | when checked, use input_U and V. otherwise use geometry dPds/t |
| Toon Specular Input U | `toon_specular_input_U` | Vec3f | 0 0 0 | can be a port | input U / tangent for specular stretch |
| Toon Specular Input V | `toon_specular_input_V` | Vec3f | 0 0 0 | can be a port | input V / bitangent for specular stretch |
| Toon Specular Ramp Input Scale | `toon_specular_ramp_input_scale` | Float | 1 | can be a port | Scales the input value x to the ramp lookup y, where x is based on the angle between the sample direction and normal.  This has the effect of squashing or stretching the ramp point positions towards/away from 0. |
| Toon Specular Ramp | `toon_specular_ramp` | ramp_float | 4 points, constant | knob only | List of colors on the ramp |
| Toon Specular Mult 0 | `toon_specular_mult0` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Toon Specular Mult 1 | `toon_specular_mult1` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Toon Specular Mult 2 | `toon_specular_mult2` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Toon Specular Mult 3 | `toon_specular_mult3` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Toon Specular Mult 4 | `toon_specular_mult4` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Toon Specular Mult 5 | `toon_specular_mult5` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Toon Specular Mult 6 | `toon_specular_mult6` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Toon Specular Mult 7 | `toon_specular_mult7` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Toon Specular Mult 8 | `toon_specular_mult8` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Toon Specular Mult 9 | `toon_specular_mult9` | Float | 1 | can be a port | Bindable multiplier on the toon specular ramp value |
| Toon Specular Offset 0 | `toon_specular_offset0` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Toon Specular Offset 1 | `toon_specular_offset1` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Toon Specular Offset 2 | `toon_specular_offset2` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Toon Specular Offset 3 | `toon_specular_offset3` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Toon Specular Offset 4 | `toon_specular_offset4` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Toon Specular Offset 5 | `toon_specular_offset5` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Toon Specular Offset 6 | `toon_specular_offset6` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Toon Specular Offset 7 | `toon_specular_offset7` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Toon Specular Offset 8 | `toon_specular_offset8` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Toon Specular Offset 9 | `toon_specular_offset9` | Float | 0 | can be a port | Bindable offset to the toon specular ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Toon Specular Enable Input Normal | `toon_specular_enable_input_normal` | Bool | off | knob only | enables sampling the normal map for toon toon specular |
| Toon Specular Input Normal | `toon_specular_input_normal` | SceneObject* |  | port only | specifies an alternate shading normal for toon toon specular |
| Toon Specular Input Normal Dial | `toon_specular_input_normal_dial` | Float | 1 | can be a port | controls influence of input normal versus hair normal for toon toon specular |
| Toon Specular Enable Indirect Reflections | `toon_specular_enable_indirect_reflections` | Bool | off | knob only | enables indirect GGX reflections for toon specular model |
| Toon Specular Indirect Reflections Intensity | `toon_specular_indirect_reflections_intensity` | Float | 1 | can be a port | the intensity for the indirect reflections of the toon specular model |
| Toon Specular Indirect Reflections Roughness | `toon_specular_indirect_reflections_roughness` | Float | 0.5 | can be a port | the roughness for the indirect reflections of the toon specular model |
| Specular Lightset | `specular_lightset` | SceneObject* |  | port only | lightset to use for specular lobes |

## Iridescence

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Iridescence | `iridescence` | Float | 0 | can be a port | controls the strength of the iridescence effect |
| Iridescence Apply To | `iridescence_apply_to` | Int | primary specular | knob only | Apply iridescence to primary specular lobe or clearcoat/moisture lobe Options: primary specular, clearcoat/moisture specular. |
| Iridescence Color Control | `iridescence_color_control` | Int | use hue interpolation | knob only | use hue interpolation: automatically cycles through hue wheel, use ramp: user specified color ramp Options: use hue interpolation, use ramp. |
| Iridescence Primary Color | `iridescence_primary_color` | Rgb | 1 0 0 | can be a port | First color to interpolate from around the hue wheel |
| Iridescence Secondary Color | `iridescence_secondary_color` | Rgb | 1 0 0 | can be a port | Second color to interpolate to around the hue wheel |
| Iridescence Flip Hue Direction | `iridescence_flip_hue_direction` | Bool | off | knob only | flip interpolation around the hue wheel to counter-clockwise direction |
| Iridescence Ramp Interpolation Mode | `iridescence_ramp_interpolation_mode` | Int | RGB | knob only | RGB: lerp in RGB space which matches UI preview but can lose saturation, HSV: lerp in HSV space which preserves saturation Options: RGB, HSV. |
| Iridescence Ramp | `iridescence_ramp` | ramp_color | 7 points, smooth | knob only | List of colors on the ramp |
| Iridescence Thickness | `iridescence_thickness` | Float | 1 | can be a port | Controls how much the color spectrum is repeated |
| Iridescence Exponent | `iridescence_exponent` | Float | 1 | can be a port | Tightens or broadens the distribution of colors |
| Iridescence At 0 Incidence | `iridescence_at_0_incidence` | Float | 1 | can be a port | Iridescence effect at 0 degree viewing angle |
| Iridescence At 90 Incidence | `iridescence_at_90_incidence` | Float | 1 | can be a port | Iridescence effect at 90 degree viewing angle |

## Diffuse

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Diffuse Lightset | `diffuse_lightset` | SceneObject* |  | port only | lightset to use for diffuse lobes |
| Show Diffuse | `show_diffuse` | Bool | on | knob only | enables/disables diffuse reflectance |
| Diffuse Model | `diffuse_model` | Int | oren-nayar | knob only | The method used to render the diffuse response. Options: oren-nayar, ramp. |
| Albedo | `albedo` | Rgb | 1 1 1 | port | the overall surface color as seen from a distance (ie. diffuse color) |
| Diffuse Roughness | `diffuse_roughness` | Float | 0 | port | Roughness of the diffuse shading.  If the value is zero a Lambertian model is used.  If it's above zero the Oren Nayar model is used.   Not compatible with subsurface scattering. |
| Terminator Shift | `terminator_shift` | Float | 0.05 | port | Controls how the diffuse ligthing falls off.  Values greater than 0.0 shift the falloff point closer to the light source and values less than 0.0 shift the falloff point further away |
| Diffuse Flatness | `diffuse_flatness` | Float | 0 | can be a port | Flattens out the diffuse response by bending the normal towards the light direction |
| Diffuse Flatness Falloff | `diffuse_flatness_falloff` | Float | 0 | can be a port | Fades out flatness with respect to light direction |
| Extend Ramp | `extend_ramp` | Bool | off | knob only | Extends the last ramp color to unlit portions. IMPORTANT: Only works in conjuction with prevent_light_culling ON and visible_shadows OFF |
| Ramp Input Scale | `ramp_input_scale` | Float | 1 | port | Factor to apply to the input diffuse reflectance before using it as the input to the ramp |
| Ramp | `ramp` | ramp_color | 4 points, constant | knob only | List of colors on the ramp |
| Ramp Color Multiplier 0 | `ramp_color_multiplier0` | Rgb | 1 1 1 | can be a port | Bindable multiplier on the ramp color |
| Ramp Color Multiplier 1 | `ramp_color_multiplier1` | Rgb | 1 1 1 | can be a port | Bindable multiplier on the ramp color |
| Ramp Color Multiplier 2 | `ramp_color_multiplier2` | Rgb | 1 1 1 | can be a port | Bindable multiplier on the ramp color |
| Ramp Color Multiplier 3 | `ramp_color_multiplier3` | Rgb | 1 1 1 | can be a port | Bindable multiplier on the ramp color |
| Ramp Color Multiplier 4 | `ramp_color_multiplier4` | Rgb | 1 1 1 | can be a port | Bindable multiplier on the ramp color |
| Ramp Color Multiplier 5 | `ramp_color_multiplier5` | Rgb | 1 1 1 | can be a port | Bindable multiplier on the ramp color |
| Ramp Color Multiplier 6 | `ramp_color_multiplier6` | Rgb | 1 1 1 | can be a port | Bindable multiplier on the ramp color |
| Ramp Color Multiplier 7 | `ramp_color_multiplier7` | Rgb | 1 1 1 | can be a port | Bindable multiplier on the ramp color |
| Ramp Color Multiplier 8 | `ramp_color_multiplier8` | Rgb | 1 1 1 | can be a port | Bindable multiplier on the ramp color |
| Ramp Color Multiplier 9 | `ramp_color_multiplier9` | Rgb | 1 1 1 | can be a port | Bindable multiplier on the ramp color |
| Ramp Position Offset 0 | `ramp_position_offset0` | Float | 0 | can be a port | Bindable offset to the ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Ramp Position Offset 1 | `ramp_position_offset1` | Float | 0 | can be a port | Bindable offset to the ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Ramp Position Offset 2 | `ramp_position_offset2` | Float | 0 | can be a port | Bindable offset to the ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Ramp Position Offset 3 | `ramp_position_offset3` | Float | 0 | can be a port | Bindable offset to the ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Ramp Position Offset 4 | `ramp_position_offset4` | Float | 0 | can be a port | Bindable offset to the ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Ramp Position Offset 5 | `ramp_position_offset5` | Float | 0 | can be a port | Bindable offset to the ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Ramp Position Offset 6 | `ramp_position_offset6` | Float | 0 | can be a port | Bindable offset to the ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Ramp Position Offset 7 | `ramp_position_offset7` | Float | 0 | can be a port | Bindable offset to the ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Ramp Position Offset 8 | `ramp_position_offset8` | Float | 0 | can be a port | Bindable offset to the ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| Ramp Position Offset 9 | `ramp_position_offset9` | Float | 0 | can be a port | Bindable offset to the ramp-position, add a small variation using noise etc for art-directable ramp thresholds |
| BSSRDF | `bssrdf` | Int | normalized diffusion | knob only | 0 for NormalizedDiffuse, 1 for Dipole, 2 for random walk Options: normalized diffusion, dipole, random walk. |
| Scattering Color | `scattering_color` | Rgb | 1 1 1 | can be a port | the subsurface scattering 'falloff' color |
| Scattering Radius | `scattering_radius` | Float | 0 | can be a port | the distance the light scatters beneath the surface. When 0 surface diffuse (lambertian or toon) is used |
| Crease Attenuation | `crease_attenuation` | Float | 0 | can be a port | This mitigates the over-brightening around creases which random walk SSS can cause. |
| Enable SSS Input Normal | `enable_sss_input_normal` | Bool | off | knob only | enables sampling the normal map for sss samples. More accurate but potentially expensive |
| Resolve Self Intersections | `resolve_self_intersections` | Bool | on | knob only | tries to resolve self-intersecting geometry automatically by only evaluating 'exiting' intersections for subsurface evaluations |
| Diffuse Transmission | `diffuse_transmission` | Float | 1 | can be a port | multiplier on the amount of light that is transmitted through the surface. |
| Diffuse Transmission Color | `diffuse_transmission_color` | Rgb | 0 0 0 | can be a port | the color/amount of light that is transmitted through the surface. |
| Diffuse Transmission Blending Behavior | `diffuse_transmission_blending_behavior` | Int | Monochromatic | knob only | Controls how diffuse transmission color attenuates diffuse reflection Options: RGB, Monochromatic. |

## Transmission

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Transmission | `transmission` | Float | 0 | can be a port | enables/disables refractive solid model (binary 0\|1 for plausibility) |
| Show Transmission | `show_transmission` | Bool | on | knob only | enables/disables refractive solid model |
| Transmission Color | `transmission_color` | Rgb | 1 1 1 | can be a port | the desired color of transmitted light |
| Use Independent Transmission Refractive Index | `use_independent_transmission_refractive_index` | Bool | off | knob only | use a separate IOR for transmission |
| Independent Transmission Refractive Index | `independent_transmission_refractive_index` | Float | 1.5 | knob only | defines a separate IOR for the bending of light with transmission |
| Use Independent Transmission Roughness | `use_independent_transmission_roughness` | Bool | off | knob only | use a separate roughness for transmission |
| Independent Transmission Roughness | `independent_transmission_roughness` | Float | 0.5 | can be a port | separate roughness for transmission |
| Use Dispersion | `use_dispersion` | Bool | off | knob only | use dispersion effects in transmission |
| Dispersion Abbe Number | `dispersion_abbe_number` | Float | 34 | knob only | The amount of dispersion/chromatic-aberration via refractions. Lower this number to increase the effect. A value of 0 turns off dispersion. Around [25-80] makes sense for realistic glass. Lower values may look better on gemstones. |

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
| Prevent Light Culling | `prevent_light_culling` | Bool | off | knob only | WARNING: Experimental and must be used with max_depth: 0 and only for non-photoreal looks. Prevents culling of lights so surfaces can be lit purely with respect to the shading normal irrespective of geometry |
| SSS Trace Set | `sss_trace_set` | SceneObject* |  | port only | Set of geometries that contribute neighboring subsurface points. By default, only the geometry associated with this material contributes to subsurface. If you want adjacent geometry with different material to contribute as well, specify all those parts here. |

## Other

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Extra AOVs | `extra_aovs` | SceneObject* |  | port only | Bind this attribute to a 'ListMap' that contains references to ExtraAovMaps that specify additional outputs that can be assigned to a RenderOutput "light aov" result |
| Label | `label` | String |  | knob only | label used in material and light aovs |
| Priority | `priority` | Int | 0 | knob only | The material's place in an order of precedence for overlapping dielectrics. A value of 0 means the priority should be ignored. Materials with lower numbers (higher priority) "override" materials with higher numbers (lower priority).  To enable automatic removal of self-overlapping geometry, a non-zero priority must be set on the geometry's material. |
| Record Reflected Cryptomatte | `record_reflected_cryptomatte` | Bool | off | knob only | Indicates whether the next reflected surface should appear in the reflected cryptomatte layers |
| Record Refracted Cryptomatte | `record_refracted_cryptomatte` | Bool | off | knob only | Indicates whether the next refracted surface should appear in the refracted cryptomatte layers |
