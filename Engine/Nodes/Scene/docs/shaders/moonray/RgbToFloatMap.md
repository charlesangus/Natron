# RgbToFloatMap

MoonRay map `RgbToFloatMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Input | `input` | Rgb | 1 1 1 | port | Input color to convert |
| Mode | `mode` | Int | average | knob only | The method to convert RGB Color to float Options: r, g, b, min, max, average, sum, luminance. |
