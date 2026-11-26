# Ray Switch Material

MoonRay material `RaySwitchMaterial`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Material |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Extra AOVs | `extra_aovs` | SceneObject* |  | port only | Bind this attribute to a 'ListMap' that contains references to ExtraAovMaps that specify additional outputs that can be assigned to a RenderOutput "light aov" result |
| Label | `label` | String |  | knob only | label used in material and light aovs |
| Priority | `priority` | Int | 0 | knob only | The material's place in an order of precedence for overlapping dielectrics. A value of 0 means the priority should be ignored. Materials with lower numbers (higher priority) "override" materials with higher numbers (lower priority).  To enable automatic removal of self-overlapping geometry, a non-zero priority must be set on the geometry's material. |
| Record Reflected Cryptomatte | `record_reflected_cryptomatte` | Bool | off | knob only | Indicates whether the next reflected surface should appear in the reflected cryptomatte layers |
| Record Refracted Cryptomatte | `record_refracted_cryptomatte` | Bool | off | knob only | Indicates whether the next refracted surface should appear in the refracted cryptomatte layers |
| Default Material | `default_material` | SceneObject* |  | port only | The default material to use if no material was specified for the incoming ray type. |
| Camera Ray Material | `camera_ray_material` | SceneObject* |  | port only | The material to use if the incoming ray is a camera ray. |
| Indirect Mirror Ray Material | `indirect_mirror_ray_material` | SceneObject* |  | port only | The material to use if the incoming ray is an indirect mirror ray. |
| Indirect Glossy Ray Material | `indirect_glossy_ray_material` | SceneObject* |  | port only | The material to use if the incoming ray is an indirect glossy ray. |
| Indirect Diffuse Ray Material | `indirect_diffuse_ray_material` | SceneObject* |  | port only | The material to use if the incoming ray is an indirect diffuse ray. |
| Cutout Camera Rays | `cutout_camera_rays` | Bool | off | knob only | If true, this material acts as a cutout material, i.e. we terminate any rays that encounter this material. |
