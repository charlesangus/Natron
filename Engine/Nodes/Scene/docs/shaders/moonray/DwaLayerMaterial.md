# Dwa Layer Material

MoonRay material `DwaLayerMaterial`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | DwaBaseLayerable |

## Advanced

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Fallback BSSRDF | `fallback_bssrdf` | Int | normalized diffusion | knob only | If child materials disagree on the type of bssrdf, this type will be used instead. Options: normalized diffusion, dipole, random walk. |
| Fallback Specular Model | `fallback_specular_model` | Int | GGX | knob only | If child materials disagree on the type of specular model, this type will be used instead. Options: Beckmann, GGX. |
| Fallback Outer Specular Model | `fallback_outer_specular_model` | Int | GGX | knob only | If child materials disagree on the type of outer specular model, this type will be used instead. Options: Beckmann, GGX. |
| Fallback Clearcoat Use Bending | `fallback_clearcoat_use_bending` | Bool | on | knob only | If child materials disagree on the type of clearcoat use bending, this type will be used instead. |
| Fallback Thin Geometry | `fallback_thin_geometry` | Bool | on | knob only | If child materials disagree on the type of thin geometry, this type will be used instead. |
| Fallback Prevent Light Culling | `fallback_prevent_light_culling` | Bool | off | knob only | If child materials disagree on prevent light culling, this will be used instead. |
| Blend Color Space | `blend_color_space` | Int | RGB | knob only | Color space used when blending the two material's color parameters Options: RGB, HSV, HSL. |
| SSS Trace Set | `sss_trace_set` | SceneObject* |  | port only | By default, only the geometry associated with this material contributes to subsurface. The DwaLayerMaterial ignores the sss trace sets of the submaterials. If you want adjacent geometry with different material to contribute as well, specify all those parts here. |

## Glitter Fallback

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Fallback Glitter Seed | `fallback_glitter_seed` | Int | 0 | knob only | The seed for the glitter random number generator.  This parameter will only be used when layering two distinct glitter materials. |
| Fallback Glitter Space | `fallback_glitter_space` | Int | reference | knob only | The space to calculate the worley noise in, defaults to reference space.  This parameter will only be used when layering two distinct glitter materials. Options: object, reference. |
| Fallback Glitter Randomness | `fallback_glitter_randomness` | Float | 0.5 | knob only | randomness of flake orientation.  This parameter will only be used when layering two distinct glitter materials. |
| Fallback Glitter Style A Frequency | `fallback_glitter_style_A_frequency` | Float | 1 | knob only | 0 implies none of this style, 1 implies all the flakes will get this style.  This parameter will only be used when layering two distinct glitter materials. |
| Fallback Glitter Texture A | `fallback_glitter_texture_A` | String |  | knob only | filename that points to a texture .exr or .tx file (must be mip-mapped and tiled with maketx).  This parameter will only be used when layering two distinct glitter materials. |
| Fallback Glitter Style B Frequency | `fallback_glitter_style_B_frequency` | Float | 1 | port | 0 implies none of this style, 1 implies all the flakes will get this style.  This parameter will only be used when layering two distinct glitter materials. |
| Fallback Glitter Texture B | `fallback_glitter_texture_B` | String |  | knob only | filename that points to a texture .exr or .tx file (must be mip-mapped and tiled with maketx).  This parameter will only be used when layering two distinct glitter materials. |
| Fallback Glitter LOD Quality | `fallback_glitter_LOD_quality` | Float | 0.5 | knob only | controls quality of glitter at distances where individual flakes cannot be perceived; at lower values, approximation kicks in earlier.  This parameter will only be used when layering two distinct glitter materials. |
| Fallback Glitter Layering Mode | `fallback_glitter_layering_mode` | Int | physical | knob only | layering mode for glitter on top of the under material. physical: conserves energy and glitter attenuates under material, additive: breaks energy conservation but glitter is never darker than the under material (eg. use case: snow).  This parameter will only be used when layering two distinct glitter materials. Options: physical, additive. |
| Fallback Glitter Debug Mode | `fallback_glitter_debug_mode` | Int | off | knob only | developer debug visualization modes.  This parameter will only be used when layering two distinct glitter materials. Options: off, blend, color, averageColor, footprintArea, radius. |

## Other

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Extra AOVs | `extra_aovs` | SceneObject* |  | port only | Bind this attribute to a 'ListMap' that contains references to ExtraAovMaps that specify additional outputs that can be assigned to a RenderOutput "light aov" result |
| Label | `label` | String |  | knob only | label used in material and light aovs |
| Priority | `priority` | Int | 0 | knob only | The material's place in an order of precedence for overlapping dielectrics. A value of 0 means the priority should be ignored. Materials with lower numbers (higher priority) "override" materials with higher numbers (lower priority).  To enable automatic removal of self-overlapping geometry, a non-zero priority must be set on the geometry's material. |
| Record Reflected Cryptomatte | `record_reflected_cryptomatte` | Bool | off | knob only | Indicates whether the next reflected surface should appear in the reflected cryptomatte layers |
| Record Refracted Cryptomatte | `record_refracted_cryptomatte` | Bool | off | knob only | Indicates whether the next refracted surface should appear in the refracted cryptomatte layers |
| Material A | `material_A` | SceneObject* |  | port only | foreground material |
| Material B | `material_B` | SceneObject* |  | port only | background material |
| Mask | `mask` | Float | 1 | port | foreground material weight |
