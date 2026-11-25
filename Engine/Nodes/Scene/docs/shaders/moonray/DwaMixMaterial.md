# DwaMixMaterial

MoonRay material `DwaMixMaterial`, for the moonray target.

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
| Fallback Glitter Style B Frequency | `fallback_glitter_style_B_frequency` | Float | 1 | can be a port | 0 implies none of this style, 1 implies all the flakes will get this style.  This parameter will only be used when layering two distinct glitter materials. |
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
| Material 0 | `material0` | SceneObject* |  | port only | material input |
| Material 1 | `material1` | SceneObject* |  | port only | material input |
| Material 2 | `material2` | SceneObject* |  | port only | material input |
| Material 3 | `material3` | SceneObject* |  | port only | material input |
| Material 4 | `material4` | SceneObject* |  | port only | material input |
| Material 5 | `material5` | SceneObject* |  | port only | material input |
| Material 6 | `material6` | SceneObject* |  | port only | material input |
| Material 7 | `material7` | SceneObject* |  | port only | material input |
| Material 8 | `material8` | SceneObject* |  | port only | material input |
| Material 9 | `material9` | SceneObject* |  | port only | material input |
| Material 10 | `material10` | SceneObject* |  | port only | material input |
| Material 11 | `material11` | SceneObject* |  | port only | material input |
| Material 12 | `material12` | SceneObject* |  | port only | material input |
| Material 13 | `material13` | SceneObject* |  | port only | material input |
| Material 14 | `material14` | SceneObject* |  | port only | material input |
| Material 15 | `material15` | SceneObject* |  | port only | material input |
| Material 16 | `material16` | SceneObject* |  | port only | material input |
| Material 17 | `material17` | SceneObject* |  | port only | material input |
| Material 18 | `material18` | SceneObject* |  | port only | material input |
| Material 19 | `material19` | SceneObject* |  | port only | material input |
| Material 20 | `material20` | SceneObject* |  | port only | material input |
| Material 21 | `material21` | SceneObject* |  | port only | material input |
| Material 22 | `material22` | SceneObject* |  | port only | material input |
| Material 23 | `material23` | SceneObject* |  | port only | material input |
| Material 24 | `material24` | SceneObject* |  | port only | material input |
| Material 25 | `material25` | SceneObject* |  | port only | material input |
| Material 26 | `material26` | SceneObject* |  | port only | material input |
| Material 27 | `material27` | SceneObject* |  | port only | material input |
| Material 28 | `material28` | SceneObject* |  | port only | material input |
| Material 29 | `material29` | SceneObject* |  | port only | material input |
| Material 30 | `material30` | SceneObject* |  | port only | material input |
| Material 31 | `material31` | SceneObject* |  | port only | material input |
| Material 32 | `material32` | SceneObject* |  | port only | material input |
| Material 33 | `material33` | SceneObject* |  | port only | material input |
| Material 34 | `material34` | SceneObject* |  | port only | material input |
| Material 35 | `material35` | SceneObject* |  | port only | material input |
| Material 36 | `material36` | SceneObject* |  | port only | material input |
| Material 37 | `material37` | SceneObject* |  | port only | material input |
| Material 38 | `material38` | SceneObject* |  | port only | material input |
| Material 39 | `material39` | SceneObject* |  | port only | material input |
| Material 40 | `material40` | SceneObject* |  | port only | material input |
| Material 41 | `material41` | SceneObject* |  | port only | material input |
| Material 42 | `material42` | SceneObject* |  | port only | material input |
| Material 43 | `material43` | SceneObject* |  | port only | material input |
| Material 44 | `material44` | SceneObject* |  | port only | material input |
| Material 45 | `material45` | SceneObject* |  | port only | material input |
| Material 46 | `material46` | SceneObject* |  | port only | material input |
| Material 47 | `material47` | SceneObject* |  | port only | material input |
| Material 48 | `material48` | SceneObject* |  | port only | material input |
| Material 49 | `material49` | SceneObject* |  | port only | material input |
| Material 50 | `material50` | SceneObject* |  | port only | material input |
| Material 51 | `material51` | SceneObject* |  | port only | material input |
| Material 52 | `material52` | SceneObject* |  | port only | material input |
| Material 53 | `material53` | SceneObject* |  | port only | material input |
| Material 54 | `material54` | SceneObject* |  | port only | material input |
| Material 55 | `material55` | SceneObject* |  | port only | material input |
| Material 56 | `material56` | SceneObject* |  | port only | material input |
| Material 57 | `material57` | SceneObject* |  | port only | material input |
| Material 58 | `material58` | SceneObject* |  | port only | material input |
| Material 59 | `material59` | SceneObject* |  | port only | material input |
| Material 60 | `material60` | SceneObject* |  | port only | material input |
| Material 61 | `material61` | SceneObject* |  | port only | material input |
| Material 62 | `material62` | SceneObject* |  | port only | material input |
| Material 63 | `material63` | SceneObject* |  | port only | material input |
| Mix | `mix` | Float | 0 | port | Which of the 64 inputs (0 to 63) to use. Fractional values will mix the two materials the value lies between |
| Remap Mix To Inputs | `remap_mix_to_inputs` | Bool | on | knob only | When enabled, multiplies mix value by number of inputs used. Inputs should start at 0 with no gaps |
| Mix Interpolation | `mix_interpolation` | Int | linear | knob only | Adjusts rate of transition from one material to the next based on mix value Options: linear, hold, nearest, smooth. |
