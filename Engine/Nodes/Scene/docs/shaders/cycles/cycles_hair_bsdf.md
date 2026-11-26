# Hair BSDF

Cycles shader `cycles_hair_bsdf`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| BSDF | closure |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Component | `component` | enum | reflection | knob only | Component Options: reflection, transmission. |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Color | `color` | color | 0.8 0.8 0.8 | port | Color |
| Offset | `offset` | float | 0 | port | Offset |
| Roughness U | `roughness_u` | float | 0.2 | port | RoughnessU |
| Roughness V | `roughness_v` | float | 0.2 | port | RoughnessV |
| Tangent | `tangent` | vector | 0 0 0 | port | Tangent |
