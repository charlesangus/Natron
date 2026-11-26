# Dwa Color Correct Material

MoonRay material `DwaColorCorrectMaterial`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | DwaBaseLayerable |

## Hue/Sat/Gain

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Hue Shift | `hue_shift` | Float | 0 | port | shifts the hue of the input (spectrum range is 0-1) |
| Saturation | `saturation` | Float | 1 | port | desaturates the input below 1.0 and adds saturation above 1.0 |
| Gain | `gain` | Float | 1 | port | multiplies the input channels by the specified value |

## TMI

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| TMI Enabled | `TMI_enabled` | Bool | off | knob only | enables the TMI parameters |
| TMI | `TMI` | Rgb | 0 0 0 | port | T = Temperature where positive values push towards blue and negative towards red.   M = Magenta where positive values push towards magenta and negative towards green.  I = Intensity where negative values remove and positive values add energy  |

## Other

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Extra AOVs | `extra_aovs` | SceneObject* |  | port only | Bind this attribute to a 'ListMap' that contains references to ExtraAovMaps that specify additional outputs that can be assigned to a RenderOutput "light aov" result |
| Label | `label` | String |  | knob only | label used in material and light aovs |
| Priority | `priority` | Int | 0 | knob only | The material's place in an order of precedence for overlapping dielectrics. A value of 0 means the priority should be ignored. Materials with lower numbers (higher priority) "override" materials with higher numbers (lower priority).  To enable automatic removal of self-overlapping geometry, a non-zero priority must be set on the geometry's material. |
| Record Reflected Cryptomatte | `record_reflected_cryptomatte` | Bool | off | knob only | Indicates whether the next reflected surface should appear in the reflected cryptomatte layers |
| Record Refracted Cryptomatte | `record_refracted_cryptomatte` | Bool | off | knob only | Indicates whether the next refracted surface should appear in the refracted cryptomatte layers |
| Input Material | `input_material` | SceneObject* |  | port only | Material to modify |
| On | `on` | Bool | on | knob only | Enable/disable all color corrections |
| Mix | `mix` | Float | 1 | port | how much of the overall color correct to mix in |
