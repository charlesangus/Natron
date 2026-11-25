# ExtraAovMap

MoonRay map `ExtraAovMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Color | `color` | Rgb | 1 1 1 | port | Bind the root of a map shader network that you want evaluated as an extra aov |
| Label | `label` | String |  | knob only | Sets the LPE label that is used for the extra aov |
| Post Scatter | `post_scatter` | Bool | off | knob only | If true, accumulate this aov when scattering off the surface as an indirect ray (after the LPE scatter transition event, after path throughput multiplication), rather than when the surface is first intersected.  The purpose of this setting is to efficiently capture information from all rays that leave a surface that could potentially intersect and trigger aov evaluation on other surfaces. |
