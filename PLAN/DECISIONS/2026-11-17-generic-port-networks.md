# Generic port networks for shaders, VOPs and channels

F2 - Native Shader Networks builds a context-neutral network core, not a shader-only one (user, 2026-11-17). There is one network node class and one container class (`NetworkBuilder`, a `NodeGroup` subclass with a set of targets). Every network node outputs the single `eDataKindNetwork`, which keeps network nodes off comp inputs. Inside a network, wiring is checked on port types alone, through `PortTypes`, where each target registers its types and implicit conversions.

Each node declares the targets it supports, which is Houdini's render-mask model. A container's Tab shows only nodes that support one of its targets. Generic operators such as Add target the kernel (VOP and channel contexts) and never appear in Cycles or MoonRay builders, which keep their renderer-native nodes. Nothing lowers generic nodes into renderer nodes. That keeps the 2026-11-05 native-network decision intact.

Kernel-target networks compile by generating M360 - Geometry Kernel Language source text, which the existing front end parses and type-checks, as Houdini generates VEX. A line-to-node table maps compile errors back to nodes, and the generated code can be shown.

Planned clients:
- E25 - Kernel Networks: geometry, particle and image VOPs, and an image wrangle.
- E26 - Channel Networks: the F32 - Channel Networks driver, its channel VOP, and channel-based animation layers.

Shipping shader-only and refactoring later was rejected. That path would have changed saved projects, and the fork keeps no backward compatibility.
