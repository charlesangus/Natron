# HairLayerMaterial

MoonRay material `HairLayerMaterial`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | DwaBaseHairLayerable |

## Advanced

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Blend Color Space | `blend_color_space` | Int | RGB | knob only | Color space used when blending the two material's color parameters Options: RGB, HSV, HSL. |
| Fallback BSSRDF | `fallback_bssrdf` | Int | normalized diffusion | knob only | If child materials disagree on the type of bssrdf, this type will be used instead. Options: normalized diffusion, dipole. |

## Other

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Extra AOVs | `extra_aovs` | SceneObject* |  | port only | Bind this attribute to a 'ListMap' that contains references to ExtraAovMaps that specify additional outputs that can be assigned to a RenderOutput "light aov" result |
| Label | `label` | String |  | knob only | label used in material and light aovs |
| Priority | `priority` | Int | 0 | knob only | The material's place in an order of precedence for overlapping dielectrics. A value of 0 means the priority should be ignored. Materials with lower numbers (higher priority) "override" materials with higher numbers (lower priority).  To enable automatic removal of self-overlapping geometry, a non-zero priority must be set on the geometry's material. |
| Record Reflected Cryptomatte | `record_reflected_cryptomatte` | Bool | off | knob only | Indicates whether the next reflected surface should appear in the reflected cryptomatte layers |
| Record Refracted Cryptomatte | `record_refracted_cryptomatte` | Bool | off | knob only | Indicates whether the next refracted surface should appear in the refracted cryptomatte layers |
| Hair Material A | `hair_material_A` | SceneObject* |  | port only | foreground hair material |
| Hair Material B | `hair_material_B` | SceneObject* |  | port only | background hair material |
| Mask | `mask` | Float | 1 | port | foreground hair material weight |
