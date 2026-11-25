# DwaFabricMaterial

MoonRay material `DwaFabricMaterial`, for the moonray target.

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

## Fabric

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Show Specular | `show_specular` | Bool | on | knob only | Enables/disables specular fabric lobes |
| Warp Color | `warp_color` | Rgb | 1 1 1 | port | Color of the fabric specular highlights. Or if 'use independent weft attributes' is 'true,' just the color of the fabric specular highlights of the warp threads. |
| Warp Roughness | `warp_roughness` | Float | 0.75 | port | Roughness of the fabric specular highlights. Or if 'use independent weft attributes' is 'true,' just the roughness of the fabric specular highlights of the warp threads. |
| Use Independent Weft Attributes | `use_independent_weft_attributes` | Bool | off | knob only | Use a separate set of attributes for the 'weft' thread specular response. |
| Weft Color | `weft_color` | Rgb | 1 1 1 | port | Color of the fabric specular highlights from the weft threads. |
| Weft Roughness | `weft_roughness` | Float | 0.75 | port | Roughness of the fabric specular highlights from the weft threads. |
| Use U Vs For Thread Direction | `use_UVs_for_thread_direction` | Bool | on | knob only | Use UV texture coordinates to compute thread direction (along dPds). Switch OFF for seamless camera aligned thread direction.  The warp_thread_direction parameter rotates this direction in tangent space. |

## Diffuse

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Fabric Diffuse Scattering | `fabric_diffuse_scattering` | Float | 0.2 | can be a port | Amount of diffuse to mix in fabric. A value of 1 means fully diffuse fabric. |
| Show Diffuse | `show_diffuse` | Bool | on | knob only | enables/disables diffuse reflectance |
| Albedo | `albedo` | Rgb | 1 1 1 | port | the overall surface color as seen from a distance (ie. diffuse color) |
| Diffuse Roughness | `diffuse_roughness` | Float | 0 | port | Roughness of the diffuse shading.  If the value is zero a Lambertian model is used.  If it's above zero the Oren Nayar model is used.   Not compatible with subsurface scattering. |
| Diffuse Transmission | `diffuse_transmission` | Float | 1 | port | multiplier on the amount of light that is transmitted through the surface. |
| Diffuse Transmission Color | `diffuse_transmission_color` | Rgb | 0 0 0 | can be a port | the color/amount of light that is transmitted through the surface. |
| Diffuse Transmission Blending Behavior | `diffuse_transmission_blending_behavior` | Int | Monochromatic | knob only | Controls how diffuse transmission color attenuates diffuse reflection Options: RGB, Monochromatic. |

## Threads

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Warp Thread Direction | `warp_thread_direction` | Vec3f | 1 1 0 | can be a port | Direction of the major/warp thread for the fabric. The fabric highlights will be relative to this direction. |
| Warp Thread Coverage | `warp_thread_coverage` | Float | 0.75 | knob only | When set to 1, creates specular highlights only relative to the warp thread direction. When less than one, starts acting like a 'woven' fabric with specular contributions added in by a corresponding 'weft' thread direction, perpendicular to the 'warp' thread direction. |
| Warp Thread Elevation | `warp_thread_elevation` | Float | 0 | can be a port | This attribute can be used to 'elevate/rotate' the threads from their default direction. When laying flat on the surface (thread_direction.z=0), a value around 45 works well for 'silky' looks. Range - -90, +90. When threads are upright (thread_direction.z=1), this attribute can be used to 'dishevel' the threads and create a 'velvety' look (see wiki). |

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

## Common

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Presence | `presence` | Float | 1 | port | controls the visibility of this object. Useful for fading an object in/out, or to specify a cut-out mask on thin single-sided geometry (eg. a complex leaf texture on a simple card). |
| Thin Geometry | `thin_geometry` | Bool | off | knob only | enables proper shading of infinitely thin geometry such as paper or leaves. |
| Casts Caustics | `casts_caustics` | Bool | off | knob only | allows continuation of caustic light paths. |

## Other

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Extra AOVs | `extra_aovs` | SceneObject* |  | port only | Bind this attribute to a 'ListMap' that contains references to ExtraAovMaps that specify additional outputs that can be assigned to a RenderOutput "light aov" result |
| Label | `label` | String |  | knob only | label used in material and light aovs |
| Priority | `priority` | Int | 0 | knob only | The material's place in an order of precedence for overlapping dielectrics. A value of 0 means the priority should be ignored. Materials with lower numbers (higher priority) "override" materials with higher numbers (lower priority).  To enable automatic removal of self-overlapping geometry, a non-zero priority must be set on the geometry's material. |
| Record Reflected Cryptomatte | `record_reflected_cryptomatte` | Bool | off | knob only | Indicates whether the next reflected surface should appear in the reflected cryptomatte layers |
| Record Refracted Cryptomatte | `record_refracted_cryptomatte` | Bool | off | knob only | Indicates whether the next refracted surface should appear in the refracted cryptomatte layers |
