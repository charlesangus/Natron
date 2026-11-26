# Metallic BSDF

Cycles shader `cycles_metallic_bsdf`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| BSDF | closure |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Distribution | `distribution` | enum | multi_ggx | knob only | Distribution Options: ggx, beckmann, multi_ggx. |
| Fresnel Type | `fresnel_type` | enum | f82 | knob only | fresnel_type Options: physical_conductor, f82. |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Color | `color` | color | 0.617 0.577 0.54 | port | Base Color |
| Normal | `normal` | normal | 0 0 0 | port | Normal |
| Edge Tint | `edge_tint` | color | 0.695 0.726 0.77 | port | Edge Tint |
| IOR | `ior` | vector | 2.757 2.513 2.231 | port | IOR |
| K | `k` | vector | 3.867 3.404 3.009 | port | Extinction |
| Tangent | `tangent` | vector | 0 0 0 | port | Tangent |
| Roughness | `roughness` | float | 0.5 | port | Roughness |
| Anisotropy | `anisotropy` | float | 0 | port | Anisotropy |
| Rotation | `rotation` | float | 0 | port | Rotation |

## Thin Film

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Thin Film Thickness | `thin_film_thickness` | float | 0 | port | Thin Film Thickness |
| Thin Film IOR | `thin_film_ior` | float | 1.33 | port | Thin Film IOR |
