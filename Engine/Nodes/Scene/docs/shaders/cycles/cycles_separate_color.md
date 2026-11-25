# Separate Color

Cycles utility `cycles_separate_color`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| r | float |
| g | float |
| b | float |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Color Type | `color_type` | enum | rgb | knob only | Type Options: rgb, hsv, hsl. |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Color | `color` | color | 0 0 0 | port | Color |
