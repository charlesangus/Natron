# Glass BSDF

Cycles shader `cycles_glass_bsdf`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| BSDF | closure |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Distribution | `distribution` | enum | ggx | knob only | Distribution Options: beckmann, ggx, multi_ggx. |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Color | `color` | color | 0.8 0.8 0.8 | port | Color |
| Normal | `normal` | normal | 0 0 0 | port | Normal |
| Roughness | `roughness` | float | 0 | port | Roughness |
| IOR | `IOR` | float | 1.5 | port | IOR |

## Thin Film

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Thin Film Thickness | `thin_film_thickness` | float | 0 | port | Thin Film Thickness |
| Thin Film IOR | `thin_film_ior` | float | 1.33 | port | Thin Film IOR |
