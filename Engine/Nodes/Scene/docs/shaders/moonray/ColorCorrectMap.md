# ColorCorrectMap

MoonRay map `ColorCorrectMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Advanced

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Use Per Channel Saturation | `use_per_channel_saturation` | Bool | off | knob only | enables separate RGB controls for saturation |
| Saturation R | `saturation_r` | Float | 1 | can be a port | desaturates the red channel input below 1.0 and adds saturation above 1.0 |
| Saturation G | `saturation_g` | Float | 1 | can be a port | desaturates the green channel input below 1.0 and adds saturation above 1.0 |
| Saturation B | `saturation_b` | Float | 1 | can be a port | desaturates the blue channel input below 1.0 and adds saturation above 1.0 |
| Use Per Channel Contrast | `use_per_channel_contrast` | Bool | off | knob only | enables separate RGB controls for contrast |
| Contrast R | `contrast_r` | Float | 0 | can be a port | negative values reduce the difference in luminance towards grey and positive values increase the difference in luminance for the red channel |
| Contrast G | `contrast_g` | Float | 0 | can be a port | negative values reduce the difference in luminance towards grey and positive values increase the difference in luminance for the green channel |
| Contrast B | `contrast_b` | Float | 0 | can be a port | negative values reduce the difference in luminance towards grey and positive values increase the difference in luminance for the blue channel |
| Use Per Channel Gamma | `use_per_channel_gamma` | Bool | off | knob only | enables separate RGB controls for gamma |
| Gamma R | `gamma_r` | Float | 1 | can be a port | raises the red channel to the specified exponents |
| Gamma G | `gamma_g` | Float | 1 | can be a port | raises the green channel to the specified exponents |
| Gamma B | `gamma_b` | Float | 1 | can be a port | raises the blue channel to the specified exponents |
| Use Per Channel Gain Offset | `use_per_channel_gain_offset` | Bool | off | knob only | enables separate RGB controls for gain and offset |
| Gain R | `gain_r` | Float | 1 | can be a port | multiplies the red channel by the specified values |
| Gain G | `gain_g` | Float | 1 | can be a port | multiplies the green channel by the specified values |
| Gain B | `gain_b` | Float | 1 | can be a port | multiplies the blue channel by the specified values |
| Offset R | `offset_r` | Float | 1 | can be a port | adds the specified values to the red channel |
| Offset G | `offset_g` | Float | 1 | can be a port | adds the specified values to the green channel |
| Offset B | `offset_b` | Float | 1 | can be a port | adds the specified values to the blue channel |

## Other

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Input | `input` | Rgb | 1 1 1 | port | bind the input here |
| On | `on` | Bool | on | knob only | enables/disables all color correct operations |
| Mix | `mix` | Float | 1 | port | how much of the overall color correct to mix in |
| Clamp | `clamp` | Bool | on | knob only | enables/disables clamping of the output values. |
| Clamp Min | `clamp_min` | Float | 0 | knob only | the minimum value output by this map when 'clamp' is enabled |
| Clamp Max | `clamp_max` | Float | 1 | knob only | the maximum value output by this map when 'clamp' is enabled |
| Hue Shift Enabled | `hue_shift_enabled` | Bool | on | knob only | enables the hue_shift parameter |
| Hue Shift | `hue_shift` | Float | 0 | port | shifts the hue of the input (spectrum range is 0-1) |
| Saturation Enabled | `saturation_enabled` | Bool | on | knob only | enables the saturation parameter |
| Saturation | `saturation` | Float | 1 | port | desaturates the input below 1.0 and adds saturation above 1.0 |
| Contrast Enabled | `contrast_enabled` | Bool | on | knob only | enables the contrast parameter |
| Contrast | `contrast` | Float | 0 | port | negative values reduce the difference in luminance towards grey and positive values increase the difference in luminance |
| Gamma Enabled | `gamma_enabled` | Bool | on | knob only | enables the gamma parameter |
| Gamma | `gamma` | Float | 1 | port | raises the input to the specified exponents |
| Gain Offset Enabled | `gain_offset_enabled` | Bool | on | knob only | enables the gain and offset parameters |
| Gain | `gain` | Float | 1 | port | multiplies the input channels by the specified values |
| Offset | `offset` | Float | 0 | port | adds the specified values to the input |
| TMI Enabled | `TMI_enabled` | Bool | on | knob only | enables the TMI parameters |
| TMI | `TMI` | Rgb | 0 0 0 | can be a port | T = Temperature where positive values push towards blue and negative towards red.   M = Magenta where positive values push towards magenta and negative towards green.  I = Intensity where negative values remove and positive values add energy  |
