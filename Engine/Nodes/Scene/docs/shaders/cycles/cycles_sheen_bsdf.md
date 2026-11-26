# Sheen BSDF

Cycles shader `cycles_sheen_bsdf`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| BSDF | closure |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Distribution | `distribution` | enum | microfiber | knob only | Distribution Options: microfiber, ashikhmin. |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Color | `color` | color | 0.8 0.8 0.8 | port | Color |
| Normal | `normal` | normal | 0 0 0 | port | Normal |
| Roughness | `roughness` | float | 1 | port | Roughness |
