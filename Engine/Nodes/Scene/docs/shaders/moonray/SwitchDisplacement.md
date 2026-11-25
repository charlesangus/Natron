# SwitchDisplacement

MoonRay displacement `SwitchDisplacement`, for the moonray target.

Generated from the MoonRay shader manifest by `tools/shaders/audit_shader_nodes.py`.

## Outputs

| Name | Type |
| --- | --- |
| out | Displacement |

## Controls

| Label | Script name | Type | Default | Port | Description |
| --- | --- | --- | --- | --- | --- |
| Bound Padding | `bound_padding` | Float | 0 | knob only | bound padding defines how much to extend the bounding box of the object. Keep this value as low as possible unless the geometry skips tessellation because control cage bounding box is out of camera frustum but the displacement stretch out of the original object bounding box (pre-displacement). Setting the bound padding too large will consume more memory and tessellation time. |
| Choice | `choice` | Float | 0 | port | Which of the 64 inputs (0 to 63) to use, values greater than 63 get cycled back to be in the [0,63] range |
| Displacement 0 | `displacement0` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 1 | `displacement1` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 2 | `displacement2` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 3 | `displacement3` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 4 | `displacement4` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 5 | `displacement5` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 6 | `displacement6` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 7 | `displacement7` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 8 | `displacement8` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 9 | `displacement9` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 10 | `displacement10` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 11 | `displacement11` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 12 | `displacement12` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 13 | `displacement13` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 14 | `displacement14` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 15 | `displacement15` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 16 | `displacement16` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 17 | `displacement17` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 18 | `displacement18` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 19 | `displacement19` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 20 | `displacement20` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 21 | `displacement21` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 22 | `displacement22` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 23 | `displacement23` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 24 | `displacement24` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 25 | `displacement25` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 26 | `displacement26` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 27 | `displacement27` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 28 | `displacement28` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 29 | `displacement29` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 30 | `displacement30` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 31 | `displacement31` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 32 | `displacement32` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 33 | `displacement33` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 34 | `displacement34` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 35 | `displacement35` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 36 | `displacement36` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 37 | `displacement37` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 38 | `displacement38` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 39 | `displacement39` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 40 | `displacement40` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 41 | `displacement41` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 42 | `displacement42` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 43 | `displacement43` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 44 | `displacement44` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 45 | `displacement45` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 46 | `displacement46` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 47 | `displacement47` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 48 | `displacement48` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 49 | `displacement49` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 50 | `displacement50` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 51 | `displacement51` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 52 | `displacement52` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 53 | `displacement53` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 54 | `displacement54` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 55 | `displacement55` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 56 | `displacement56` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 57 | `displacement57` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 58 | `displacement58` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 59 | `displacement59` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 60 | `displacement60` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 61 | `displacement61` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 62 | `displacement62` | SceneObject* |  | port only | A reference to a displacement scene object |
| Displacement 63 | `displacement63` | SceneObject* |  | port only | A reference to a displacement scene object |
