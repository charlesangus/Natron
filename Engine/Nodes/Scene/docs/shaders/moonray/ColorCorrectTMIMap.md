# Color Correct TMI Map

MoonRay map `ColorCorrectTMIMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Input | `input` | Rgb | 1 1 1 | port | bind the input here |
| On | `on` | Bool | on | knob only | enables/disables all color correct operations |
| Mix | `mix` | Float | 1 | port | how much of the overall color correct to mix in |
| TMI | `TMI` | Rgb | 0 0 0 | port | T = Temperature where positive values push towards blue and negative towards red.   M = Magenta where positive values push towards magenta and negative towards green.  I = Intensity where negative values remove and positive values add energy  |
