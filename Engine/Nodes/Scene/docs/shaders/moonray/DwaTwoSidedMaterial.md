# Dwa Two Sided Material

MoonRay material `DwaTwoSidedMaterial`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | DwaBaseLayerable |

## Advanced

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Fallback BSSRDF | `fallback_bssrdf` | Int | normalized diffusion | knob only | If the two materials disagree on the type of bssrdf, this type will be used instead. Options: normalized diffusion, dipole, random walk. |
| Fallback Specular Model | `fallback_specular_model` | Int | GGX | knob only | If child materials disagree on the type of specular model, this type will be used instead. Options: Beckmann, GGX. |
| Fallback Outer Specular Model | `fallback_outer_specular_model` | Int | GGX | knob only | If child materials disagree on the type of outer specular model, this type will be used instead. Options: Beckmann, GGX. |
| Fallback Clearcoat Use Bending | `fallback_clearcoat_use_bending` | Bool | on | knob only | If child materials disagree on the type of clearcoat use bending, this type will be used instead. |
| Fallback Prevent Light Culling | `fallback_prevent_light_culling` | Bool | off | knob only | If child materials disagree on prevent light culling, this will be used instead. |
| SSS Trace Set | `sss_trace_set` | SceneObject* |  | port only | By default, only the geometry associated with this material contributes to subsurface. The DwaTwoSidedMaterial ignores the sss trace sets of the submaterials. If you want adjacent geometry with different material to contribute as well, specify all those parts here. |

## Other

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Extra AOVs | `extra_aovs` | SceneObject* |  | port only | Bind this attribute to a 'ListMap' that contains references to ExtraAovMaps that specify additional outputs that can be assigned to a RenderOutput "light aov" result |
| Label | `label` | String |  | knob only | label used in material and light aovs |
| Priority | `priority` | Int | 0 | knob only | The material's place in an order of precedence for overlapping dielectrics. A value of 0 means the priority should be ignored. Materials with lower numbers (higher priority) "override" materials with higher numbers (lower priority).  To enable automatic removal of self-overlapping geometry, a non-zero priority must be set on the geometry's material. |
| Record Reflected Cryptomatte | `record_reflected_cryptomatte` | Bool | off | knob only | Indicates whether the next reflected surface should appear in the reflected cryptomatte layers |
| Record Refracted Cryptomatte | `record_refracted_cryptomatte` | Bool | off | knob only | Indicates whether the next refracted surface should appear in the refracted cryptomatte layers |
| Front Material | `front_material` | SceneObject* |  | port only | material to use on front-facing surfaces |
| Back Material | `back_material` | SceneObject* |  | port only | material to use on back-facing surfaces |
