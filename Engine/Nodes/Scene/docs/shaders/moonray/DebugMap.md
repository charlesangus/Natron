# DebugMap

MoonRay map `DebugMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Normal

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Input Normal Space | `input_normal_space` | Int | tangent | knob only | Specifies what space the input normal is in.  Usually this is tangent space for texture maps and render space for projections Options: tangent, render. |

## Primitive Attribute

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Primitive Attribute Name | `primitive_attribute_name` | String | `st` | knob only | the name of primitive attribute to displayed when attribute 'map type' is set to 'primitive attribute' |
| Primitive Attribute Type | `primitive_attribute_type` | Int | vec2f | knob only | the type of primitive attribute to displayed when attribute 'map type' is set to 'primitive attribute' Options: float, vec2f, vec3f, rgb. |

## Other

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Checkerboard | `checkerboard` | Bool | on | knob only |  |
| Map Type | `map_type` | Int | position | knob only | Options: position, texture st, shading normal, geometric normal, dpds, dpdt, primitive attribute. |
| Input Normal | `input_normal` | Vec3f | 0 0 1 | port |  |
| Input Normal Dial | `input_normal_dial` | Float | 1 | knob only |  |
