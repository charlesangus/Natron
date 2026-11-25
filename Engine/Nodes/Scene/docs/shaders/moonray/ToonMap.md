# ToonMap

MoonRay map `ToonMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Mode | `mode` | Int | both | knob only | Pick which features are displayed Options: outline, crease, both. |
| Fill Color | `fill_color` | Rgb | 0 0 0 | port | Default color, within the outlines and creases |
| Outline Color | `outline_color` | Rgb | 1 1 1 | port | Outlines are silhouettes of the geometry |
| Crease Color | `crease_color` | Rgb | 1 0 0 | port | Creases are sharp edges like corners in the geometry. |
| Outline Threshold | `outline_threshold` | Float | 0 | port | In most cases, the shader would trace an outline of a model well when this threshold is zero. |
| Outline Scale | `outline_scale` | Float | 1 | port | This attribute controls the thickness of outlines. |
| Crease Threshold | `crease_threshold` | Float | 45 | port | This attribute sets the threshold angle (in degree units) to draw creases. The more the threshold angle is, the less the creases are traced. |
| Crease Scale | `crease_scale` | Float | 1 | port | This attribute controls the thickness of creases. |
