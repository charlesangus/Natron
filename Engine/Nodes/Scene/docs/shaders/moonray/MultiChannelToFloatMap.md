# MultiChannelToFloatMap

MoonRay map `MultiChannelToFloatMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Input | `input` | Rgb | 1 1 1 | port |  |
| Mode | `mode` | Int | Red | knob only | Specifies which color channels including combination channels (Cyan, Magenta, Yellow, and White) to convert to float. Options: Red, Green, Blue, Cyan, Magenta, Yellow, White. |
| Tolerance | `tolerance` | Float | 0.2 | port | Low tolerance values will tend to produce harsher edges near overlapping colors, while high tolerance values may result in cross-color bleeding. |
