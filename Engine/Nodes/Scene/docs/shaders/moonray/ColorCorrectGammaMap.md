# ColorCorrectGammaMap

MoonRay map `ColorCorrectGammaMap`, for the moonray target.

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
| Gamma | `gamma` | Float | 1 | port | raises the input to the specified exponents |
| Use Per Channel Gamma | `use_per_channel_gamma` | Bool | off | knob only | enables separate RGB controls for gamma |
| Gamma R | `gamma_r` | Float | 1 | port | raises the red channel to the specified exponents |
| Gamma G | `gamma_g` | Float | 1 | port | raises the green channel to the specified exponents |
| Gamma B | `gamma_b` | Float | 1 | port | raises the blue channel to the specified exponents |
