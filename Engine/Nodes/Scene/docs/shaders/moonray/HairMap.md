# Hair Map

MoonRay map `HairMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Base Color | `base_color` | Rgb | 1 1 1 | port | Color at each hair's base |
| Tip Color | `tip_color` | Rgb | 1 1 1 | port | Color at each hair's tip |
| Bias | `bias` | Float | 0.5 | port | Modulates the shape of blending between base and tip color. < 0.5 pushes the center of the blend towards the base, and > 0.5 towards the tip. |
| Gain | `gain` | Float | 0.5 | port | Modulates the shape of blending between base and tip color. < 0.5 is higher contrast in the middle of the blend. > 0.5 is higher contrast at the start and end, and lower contrast in the middle. |
| Column UV Color | `column_uv_color` | Rgb | 1 1 1 | port | Bound image map must have a HairColumnUvMap bound to its input texture coordinates. |
