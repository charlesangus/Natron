# Switch Material

MoonRay material `SwitchMaterial`, for the moonray target.

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
| Choice | `choice` | Int | 0 | knob only | which of the 64 inputs (0 to 63) to use |
| Material 0 | `material0` | SceneObject* |  | port only | material input option |
| Material 1 | `material1` | SceneObject* |  | port only | material input option |
| Material 2 | `material2` | SceneObject* |  | port only | material input option |
| Material 3 | `material3` | SceneObject* |  | port only | material input option |
| Material 4 | `material4` | SceneObject* |  | port only | material input option |
| Material 5 | `material5` | SceneObject* |  | port only | material input option |
| Material 6 | `material6` | SceneObject* |  | port only | material input option |
| Material 7 | `material7` | SceneObject* |  | port only | material input option |
| Material 8 | `material8` | SceneObject* |  | port only | material input option |
| Material 9 | `material9` | SceneObject* |  | port only | material input option |
| Material 10 | `material10` | SceneObject* |  | port only | material input option |
| Material 11 | `material11` | SceneObject* |  | port only | material input option |
| Material 12 | `material12` | SceneObject* |  | port only | material input option |
| Material 13 | `material13` | SceneObject* |  | port only | material input option |
| Material 14 | `material14` | SceneObject* |  | port only | material input option |
| Material 15 | `material15` | SceneObject* |  | port only | material input option |
| Material 16 | `material16` | SceneObject* |  | port only | material input option |
| Material 17 | `material17` | SceneObject* |  | port only | material input option |
| Material 18 | `material18` | SceneObject* |  | port only | material input option |
| Material 19 | `material19` | SceneObject* |  | port only | material input option |
| Material 20 | `material20` | SceneObject* |  | port only | material input option |
| Material 21 | `material21` | SceneObject* |  | port only | material input option |
| Material 22 | `material22` | SceneObject* |  | port only | material input option |
| Material 23 | `material23` | SceneObject* |  | port only | material input option |
| Material 24 | `material24` | SceneObject* |  | port only | material input option |
| Material 25 | `material25` | SceneObject* |  | port only | material input option |
| Material 26 | `material26` | SceneObject* |  | port only | material input option |
| Material 27 | `material27` | SceneObject* |  | port only | material input option |
| Material 28 | `material28` | SceneObject* |  | port only | material input option |
| Material 29 | `material29` | SceneObject* |  | port only | material input option |
| Material 30 | `material30` | SceneObject* |  | port only | material input option |
| Material 31 | `material31` | SceneObject* |  | port only | material input option |
| Material 32 | `material32` | SceneObject* |  | port only | material input option |
| Material 33 | `material33` | SceneObject* |  | port only | material input option |
| Material 34 | `material34` | SceneObject* |  | port only | material input option |
| Material 35 | `material35` | SceneObject* |  | port only | material input option |
| Material 36 | `material36` | SceneObject* |  | port only | material input option |
| Material 37 | `material37` | SceneObject* |  | port only | material input option |
| Material 38 | `material38` | SceneObject* |  | port only | material input option |
| Material 39 | `material39` | SceneObject* |  | port only | material input option |
| Material 40 | `material40` | SceneObject* |  | port only | material input option |
| Material 41 | `material41` | SceneObject* |  | port only | material input option |
| Material 42 | `material42` | SceneObject* |  | port only | material input option |
| Material 43 | `material43` | SceneObject* |  | port only | material input option |
| Material 44 | `material44` | SceneObject* |  | port only | material input option |
| Material 45 | `material45` | SceneObject* |  | port only | material input option |
| Material 46 | `material46` | SceneObject* |  | port only | material input option |
| Material 47 | `material47` | SceneObject* |  | port only | material input option |
| Material 48 | `material48` | SceneObject* |  | port only | material input option |
| Material 49 | `material49` | SceneObject* |  | port only | material input option |
| Material 50 | `material50` | SceneObject* |  | port only | material input option |
| Material 51 | `material51` | SceneObject* |  | port only | material input option |
| Material 52 | `material52` | SceneObject* |  | port only | material input option |
| Material 53 | `material53` | SceneObject* |  | port only | material input option |
| Material 54 | `material54` | SceneObject* |  | port only | material input option |
| Material 55 | `material55` | SceneObject* |  | port only | material input option |
| Material 56 | `material56` | SceneObject* |  | port only | material input option |
| Material 57 | `material57` | SceneObject* |  | port only | material input option |
| Material 58 | `material58` | SceneObject* |  | port only | material input option |
| Material 59 | `material59` | SceneObject* |  | port only | material input option |
| Material 60 | `material60` | SceneObject* |  | port only | material input option |
| Material 61 | `material61` | SceneObject* |  | port only | material input option |
| Material 62 | `material62` | SceneObject* |  | port only | material input option |
| Material 63 | `material63` | SceneObject* |  | port only | material input option |
