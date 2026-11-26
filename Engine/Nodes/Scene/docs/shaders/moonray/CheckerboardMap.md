# Checkerboard Map

MoonRay map `CheckerboardMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Color A | `color_A` | Rgb | 0 0 0 | knob only |  |
| Color B | `color_B` | Rgb | 1 1 1 | knob only |  |
| Num U Tiles | `num_u_tiles` | Int | 8 | knob only | number of checkerboard squares in the U direction |
| Num V Tiles | `num_v_tiles` | Int | 8 | knob only | number of checkerboard squares in the V direction |
| Texture Coordinates | `texture_coordinates` | Int | texture | knob only | switches between the model's uv coordinates or the input texture coordinates Options: texture, input texture coordinates. |
| Input Texture Coordinates | `input_texture_coordinates` | Vec3f | 0 0 0 | port | bind a shader that outputs UV coordinates (such as a projection shader) here |
