# Subsurface Scattering

Cycles shader `cycles_subsurface_scattering`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| BSSRDF | closure |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Method | `method` | enum | random_walk | knob only | Method Options: burley, random_walk, random_walk_legacy, random_walk_skin. |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Color | `color` | color | 0.8 0.8 0.8 | port | Color |
| Normal | `normal` | normal | 0 0 0 | port | Normal |
| Scale | `scale` | float | 0.005 | port | Scale |
| Radius | `radius` | vector | 1 0.2 0.1 | port | Radius |
| Subsurface IOR | `subsurface_ior` | float | 1.4 | port | IOR |
| Subsurface Roughness | `subsurface_roughness` | float | 1 | port | Roughness |
| Subsurface Anisotropy | `subsurface_anisotropy` | float | 0 | port | Anisotropy |
