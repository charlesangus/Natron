# Toon BSDF

Cycles shader `cycles_toon_bsdf`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| BSDF | closure |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Component | `component` | enum | diffuse | knob only | Component Options: diffuse, glossy. |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Color | `color` | color | 0.8 0.8 0.8 | port | Color |
| Normal | `normal` | normal | 0 0 0 | port | Normal |
| Size | `size` | float | 0.5 | port | Size |
| Smooth | `smooth` | float | 0 | port | Smooth |
