# LayerMap

MoonRay map `LayerMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Input A | `input_A` | Rgb | 1 1 1 | port | Foreground color to blend |
| Input B | `input_B` | Rgb | 1 1 1 | port | Background color to blend |
| Mask | `mask` | Float | 1 | port | Blending amount |
| Mode | `mode` | Int | over | knob only | Method of blending Options: off, over, add, subtract, multiply, screen, overlay, overlay contrast, darken, lighten, color dodge, color burn, hard light, soft light, difference, exclusion. |
