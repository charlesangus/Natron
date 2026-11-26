# Usd Preview Surface

MoonRay material `UsdPreviewSurface`, for the moonray target.

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
| Diffuse Color | `diffuseColor` | Rgb | 0.18 0.18 0.18 | port | When using metallic workflow this is interpreted as albedo. |
| Emissive Color | `emissiveColor` | Rgb | 0 0 0 | port | Emissive component. |
| Use Specular Workflow | `useSpecularWorkflow` | Int | Metalness workflow | knob only | This node can fundamentally operate in two modes : Specular workflow where you provide a texture/value to the 'specularColor' input. Or, Metallic workflow where you provide a texture/value to the 'metallic' input. Options: Metalness workflow, Specular workflow. |
| Specular Color | `specularColor` | Rgb | 0 0 0 | port | Specular color to be used. This is the color at 0 incidence. Edge color is assumed white. Transition between the two colors according to Schlick fresnel approximation. |
| Metallic | `metallic` | Float | 0 | port | Use 1 for metallic surfaces and 0 for non-metallic.  If metallic is 1, then both F0 (reflectivity at 0 degree incidence) and edge F90 reflectivity will simply be the Albedo.  If metallic is 0, then Albedo is ignored in the calculation of F0 and F90; F0 is derived from ior via ( (1-ior)/(1+ior) )^2 and F90 is white. In between, we interpolate. |
| Roughness | `roughness` | Float | 0.5 | port | Roughness for the specular lobe. The value ranges from 0 to 1, which goes from a perfectly specular surface at 0.0 to maximum roughness of the specular lobe. |
| Clearcoat | `clearcoat` | Float | 0 | port | Second specular lobe amount. The color is white. |
| Clearcoat Roughness | `clearcoatRoughness` | Float | 0.01 | port | Roughness for the second specular lobe. |
| Opacity | `opacity` | Float | 1 | port | When opacity is 1.0 then the geometry is fully opaque, if it is smaller than 1.0 then the geometry is translucent, when it is 0 the geometry is transparent. Note that even a fully transparent object still receives lighting as, for example, perfectly clear glass still has a specular response. |
| Opacity Threshold | `opacityThreshold` | Float | 0 | can be a port | The opacityThreshold input is useful for creating geometric cut-outs based on the opacity input. A value of 0.0 indicates that no masking is applied to the opacity input, while a value greater than 0.0 indicates that rendering of the surface is limited to the areas where the opacity is greater or equal to that value. |
| IOR | `ior` | Float | 1.5 | port | Index of Refraction to be used for translucent objects and objects with specular components, including the clearcoat if clearcoat > 0. |
| Normal | `normal` | Vec3f | 0 0 1 | port | Expects normal in tangent space [(-1,-1,-1), (1,1,1)] |
| Displacement | `displacement` | Float | 0 | port | Displacement in the direction of the normal. |
| Occlusion | `occlusion` | Float | 1 | port | Ignored by Moonray |
