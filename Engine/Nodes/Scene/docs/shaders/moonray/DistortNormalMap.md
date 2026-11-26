# Distort Normal Map

MoonRay normal_map `DistortNormalMap`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | NormalMap |

## Space

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Noise Space | `noise_space` | Int | object | knob only | The space to calculate the noise in Options: world, object, reference, texture, input texture coordinates, hair_surface_uv, hair_closest_surface_uv. |
| Input Texture Coordinates | `input_texture_coordinates` | Vec3f | 0 0 0 | port |  |

## Other

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Use Input Vectors | `use_input_vectors` | Bool | off | knob only | when checked, use input_U and V. otherwise use geometry dPds/t |
| Input U | `input_U` | Rgb | 0 0 0 | port | input U / tangent for distortion |
| Input V | `input_V` | Rgb | 0 0 0 | port | input V / bitangent for distortion |
| Seed | `seed` | Int | 0 | knob only | the seed for the noise generation |
| Frequency U | `frequency_U` | Vec3f | 1 1 1 | knob only | controls noise frequency for U distortion |
| Frequency V | `frequency_V` | Vec3f | 1 1 1 | knob only | controls noise frequency for V distortion |
| Amplitude U | `amplitude_U` | Float | 1 | knob only | controls amplitude of U distortion |
| Amplitude V | `amplitude_V` | Float | 1 | knob only | controls amplitude of V distortion |
| Input Normals | `input_normals` | SceneObject* |  | port only | optional input to distort. if not connected, use geom normals |
