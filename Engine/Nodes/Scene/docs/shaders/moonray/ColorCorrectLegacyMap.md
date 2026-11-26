# Color Correct Legacy Map

MoonRay map `ColorCorrectLegacyMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Input | `input` | Rgb | 1 1 1 | port |  |
| On | `on` | Bool | on | knob only |  |
| Mask | `mask` | Float | 1 | port |  |
| Brightness | `brightness` | Float | 0 | knob only |  |
| Contrast | `contrast` | Float | 0 | knob only |  |
| Hue | `hue` | Float | 0 | knob only |  |
| Saturation | `saturation` | Float | 0 | knob only |  |
| Invert | `invert` | Bool | off | knob only |  |
| Monochrome | `monochrome` | Int | off | knob only | Options: off, luminance, average, minimum, maximum, red channel, green channel, blue channel. |
| Multiplier | `multiplier` | Rgb | 1 1 1 | knob only |  |
| Clamp | `clamp` | Bool | off | knob only |  |
