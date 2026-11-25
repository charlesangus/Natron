# AttributeMap

MoonRay map `AttributeMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Map |

## Primitive Attribute

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Primitive Attribute Name | `primitive_attribute_name` | String | `Cd` | knob only | the name of primitive attribute to display when attribute 'map type' is set to 'primitive attribute' |
| Primitive Attribute Type | `primitive_attribute_type` | Int | rgb | knob only | the type of primitive attribute to display when attribute 'map type' is set to 'primitive attribute' Options: float, vec2f, vec3f, rgb, int. |

## Other

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Map Type | `map_type` | Int | primitive attribute | knob only | Options: primitive attribute, position, texture st, shading normal, geometric normal, dpds, dpdt, dnds, dndt, map color, hair surface P, hair surface N, hair surface st, hair closest surface st, id, velocity, acceleration, motionvec, observer direction. |
| Default Value | `default_value` | Rgb | 1 1 1 | port | default value to display when the requested attribute is not available |
| Warn When Unavailable | `warn_when_unavailable` | Bool | off | knob only | Whether or not to issue a warning when the requested attribute is unavailable |
| Color | `color` | Rgb | 1 1 1 | port | input color - preferably a connected map |
