# Dwa Adjust Material

MoonRay material `DwaAdjustMaterial`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | DwaBaseLayerable |

## Enable

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Adjust Specular | `adjust_specular` | Bool | on | knob only | use specular adjustment attrs: specular_set, specular_set_blend, specular_mult |
| Adjust Roughness | `adjust_roughness` | Bool | on | knob only | use roughness adjustment attrs: roughness_set, roughness_set_blend, roughness_mult, roughness_remap_{in/out}_{min/max} |
| Adjust Color | `adjust_color` | Bool | on | knob only | use color adjustment attrs: color_hue_shift, color_saturation, color_gain |
| Adjust Presence | `adjust_presence` | Bool | on | knob only | use presence adjustment attrs: presence_set, presence_set_blend, presence_mult |

## Override

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Casts Caustics | `casts_caustics` | Int | unmodified | knob only | allows you to keep or set casts caustics attribute Options: unmodified, force on, force off. |
| Thin Geometry | `thin_geometry` | Int | unmodified | knob only | allows you to keep or set thin geometry attribute Options: unmodified, force on, force off. |
| Disable Diffuse | `disable_diffuse` | Bool | off | knob only | when checked, turn off all diffuse from input |
| Disable Clearcoat | `disable_clearcoat` | Bool | off | knob only | when checked, turn off clearcoat from input |
| Disable Specular | `disable_specular` | Bool | off | knob only | when checked, turn off all specular from input |

## Other

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Extra AOVs | `extra_aovs` | SceneObject* |  | port only | Bind this attribute to a 'ListMap' that contains references to ExtraAovMaps that specify additional outputs that can be assigned to a RenderOutput "light aov" result |
| Label | `label` | String |  | knob only | label used in material and light aovs |
| Priority | `priority` | Int | 0 | knob only | The material's place in an order of precedence for overlapping dielectrics. A value of 0 means the priority should be ignored. Materials with lower numbers (higher priority) "override" materials with higher numbers (lower priority).  To enable automatic removal of self-overlapping geometry, a non-zero priority must be set on the geometry's material. |
| Record Reflected Cryptomatte | `record_reflected_cryptomatte` | Bool | off | knob only | Indicates whether the next reflected surface should appear in the reflected cryptomatte layers |
| Record Refracted Cryptomatte | `record_refracted_cryptomatte` | Bool | off | knob only | Indicates whether the next refracted surface should appear in the refracted cryptomatte layers |
| Input Material | `input_material` | SceneObject* |  | port only | Material to modify |
| On | `on` | Bool | on | knob only | Enable/disable all adjustments |
| Mix | `mix` | Float | 1 | port | weight of adjustments applied to the material |
| Emission Mode | `emission_mode` | Int | off | knob only | how to handle emission input. masked uses mix input, unmasked is mix = 1 Options: off, masked, unmasked. |
| Emission | `emission` | Rgb | 1 1 1 | port | emissive map to add to material's emission |
