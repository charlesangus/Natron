# Principled BSDF

Cycles shader `cycles_principled_bsdf`, for the cycles target.

Generated from the Cycles shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| BSDF | closure |

## Base

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Base Color | `base_color` | color | 0.8 0.8 0.8 | port | Base Color |
| Metallic | `metallic` | float | 0 | port | Metallic |
| Roughness | `roughness` | float | 0.5 | port | Roughness |
| IOR | `ior` | float | 1.5 | port | IOR |
| Alpha | `alpha` | float | 1 | port | Alpha |
| Normal | `normal` | normal | 0 0 0 | port | Normal |

## Diffuse

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Diffuse Roughness | `diffuse_roughness` | float | 0 | can be a port | Diffuse Roughness |

## Subsurface

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Subsurface Method | `subsurface_method` | enum | random_walk | knob only | Subsurface Method Options: burley, random_walk, random_walk_legacy, random_walk_skin. |
| Subsurface Weight | `subsurface_weight` | float | 0 | port | Subsurface Weight |
| Subsurface Scale | `subsurface_scale` | float | 0.005 | can be a port | Subsurface Scale |
| Subsurface Radius | `subsurface_radius` | vector | 1 0.2 0.1 | can be a port | Subsurface Radius |
| Subsurface IOR | `subsurface_ior` | float | 1.4 | can be a port | Subsurface IOR |
| Subsurface Anisotropy | `subsurface_anisotropy` | float | 0 | can be a port | Subsurface Anisotropy |

## Specular

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Distribution | `distribution` | enum | multi_ggx | knob only | Distribution Options: ggx, multi_ggx. |
| Specular IOR Level | `specular_ior_level` | float | 0.5 | port | Specular IOR Level |
| Specular Tint | `specular_tint` | color | 1 1 1 | can be a port | Specular Tint |
| Anisotropic | `anisotropic` | float | 0 | can be a port | Anisotropic |
| Anisotropic Rotation | `anisotropic_rotation` | float | 0 | can be a port | Anisotropic Rotation |
| Tangent | `tangent` | normal | 0 0 0 | can be a port | Tangent |

## Transmission

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Thin Wall | `thin_wall` | int | 0 | can be a port | Thin Wall |
| Transmission Weight | `transmission_weight` | float | 0 | port | Transmission Weight |

## Coat

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Coat Weight | `coat_weight` | float | 0 | port | Coat Weight |
| Coat Roughness | `coat_roughness` | float | 0.03 | can be a port | Coat Roughness |
| Coat IOR | `coat_ior` | float | 1.5 | can be a port | Coat IOR |
| Coat Tint | `coat_tint` | color | 1 1 1 | can be a port | Coat Tint |
| Coat Normal | `coat_normal` | normal | 0 0 0 | can be a port | Coat Normal |

## Sheen

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Sheen Weight | `sheen_weight` | float | 0 | port | Sheen Weight |
| Sheen Roughness | `sheen_roughness` | float | 0.5 | can be a port | Sheen Roughness |
| Sheen Tint | `sheen_tint` | color | 1 1 1 | can be a port | Sheen Tint |

## Emission

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Emission Color | `emission_color` | color | 1 1 1 | port | Emission Color |
| Emission Strength | `emission_strength` | float | 0 | can be a port | Emission Strength |

## Thin Film

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Thin Film Thickness | `thin_film_thickness` | float | 0 | can be a port | Thin Film Thickness |
| Thin Film IOR | `thin_film_ior` | float | 1.33 | can be a port | Thin Film IOR |
