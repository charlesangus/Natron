# Principled Hair BSDF

Cycles shader `cycles_principled_hair_bsdf`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| BSDF | closure |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Model | `model` | enum | Huang | knob only | Model Options: Chiang, Huang. |
| Parametrization | `parametrization` | enum | Direct coloring | knob only | Parametrization Options: Direct coloring, Melanin concentration, Absorption coefficient. |

## Color

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Color | `color` | color | 0.017513 0.005763 0.002059 | port | Color |
| Melanin | `melanin` | float | 0.8 | port | Melanin |
| Melanin Redness | `melanin_redness` | float | 1 | port | Melanin Redness |
| Tint | `tint` | color | 1 1 1 | port | Tint |
| Absorption Coefficient | `absorption_coefficient` | vector | 0.245531 0.52 1.365 | port | Absorption Coefficient |
| Random Color | `random_color` | float | 0 | port | Random Color |

## Roughness

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Roughness | `roughness` | float | 0.3 | port | Roughness |
| Radial Roughness | `radial_roughness` | float | 0.3 | port | Radial Roughness |
| Coat | `coat` | float | 0 | port | Coat |
| Random Roughness | `random_roughness` | float | 0 | port | Random Roughness |

## Geometry

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Aspect Ratio | `aspect_ratio` | float | 0.85 | can be a port | Aspect Ratio |
| Offset | `offset` | float | 0.0349066 | port | Offset |
| IOR | `ior` | float | 1.55 | port | IOR |

## Lobes

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| R | `R` | float | 1 | can be a port | R lobe |
| TT | `TT` | float | 1 | can be a port | TT lobe |
| TRT | `TRT` | float | 1 | can be a port | TRT lobe |

## Random

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Random | `random` | float | 0 | can be a port | Random |
