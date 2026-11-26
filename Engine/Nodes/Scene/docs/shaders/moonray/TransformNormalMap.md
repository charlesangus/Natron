# Transform Normal Map

MoonRay map `TransformNormalMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Normal

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Input Normal | `input_normal` | Vec3f | 0 0 1 | port | input normal in either tangent or render space |

## Other

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Transform | `transform` | Int | tangent to render | knob only | transform to apply to the normals Options: tangent to render, render to tangent. |
| Decode Input Normal | `decode_input_normal` | Bool | on | knob only | decode the input normal if it's in tangent space [0,1] -> [-1,1] |
