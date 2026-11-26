# Principled Volume

Cycles shader `cycles_principled_volume`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| volume | closure |

## Volume

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Color | `color` | color | 0.5 0.5 0.5 | port | Color |
| Density | `density` | float | 1 | port | Density |
| Anisotropy | `anisotropy` | float | 0 | port | Anisotropy |
| Absorption Color | `absorption_color` | color | 0 0 0 | port | Absorption Color |

## Emission

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Emission Strength | `emission_strength` | float | 0 | port | Emission Strength |
| Emission Color | `emission_color` | color | 1 1 1 | port | Emission Color |

## Blackbody

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Blackbody Intensity | `blackbody_intensity` | float | 0 | port | Blackbody Intensity |
| Blackbody Tint | `blackbody_tint` | color | 1 1 1 | port | Blackbody Tint |
| Temperature | `temperature` | float | 1000 | port | Temperature |

## Attributes

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Density Attribute | `density_attribute` | string |  | port | Density Attribute |
| Color Attribute | `color_attribute` | string |  | port | Color Attribute |
| Temperature Attribute | `temperature_attribute` | string |  | port | Temperature Attribute |
