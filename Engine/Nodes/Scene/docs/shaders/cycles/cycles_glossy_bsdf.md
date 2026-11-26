# Glossy BSDF

Cycles shader `cycles_glossy_bsdf`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| BSDF | closure |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Distribution | `distribution` | enum | ggx | knob only | Distribution Options: ggx, beckmann, multi_ggx, ashikhmin_shirley. |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Color | `color` | color | 0.8 0.8 0.8 | port | Color |
| Normal | `normal` | normal | 0 0 0 | port | Normal |
| Tangent | `tangent` | vector | 0 0 0 | port | Tangent |
| Roughness | `roughness` | float | 0.5 | port | Roughness |
| Anisotropy | `anisotropy` | float | 0 | port | Anisotropy |
| Rotation | `rotation` | float | 0 | port | Rotation |
