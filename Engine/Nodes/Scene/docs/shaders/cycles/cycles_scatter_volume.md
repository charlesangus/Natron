# Scatter Volume

Cycles shader `cycles_scatter_volume`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| volume | closure |

## Settings

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Phase | `phase` | enum | Henyey-Greenstein | knob only | Phase Options: Henyey-Greenstein, Mie, Fournier-Forand, Rayleigh, Draine. |

## Inputs

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Color | `color` | color | 0.8 0.8 0.8 | port | Color |
| Density | `density` | float | 1 | port | Density |
| Anisotropy | `anisotropy` | float | 0 | port | Anisotropy |
| IOR | `IOR` | float | 1.33 | port | IOR |
| Backscatter | `backscatter` | float | 0.1 | port | Backscatter |
| Alpha | `alpha` | float | 0.5 | port | Alpha |
| Diameter | `diameter` | float | 20 | port | Diameter |
