# M488 - Channel Operators And Channel VOP

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

The operator set for F32 - Channel Networks. It covers Math (combine channels and sets), Lag, Filter (box and Gaussian smoothing), Spring, Limit, Shift, Trim, Stretch, Resample, Cycle, Slope, Area, Merge, Rename, Delete, Switch, Blend, Layer Mix (weighted layer composition, which M453 - Follow Animation Layer uses) and Expression. It also adds a channel `KernelBinding` with `ChannelWrangle` (text) and `ChannelVOP` (a kernel network on M482 - Geometry VOP Node's library). Both run per channel over all samples, with `@V`, `@I`, `@C`, `@Frame` and `@Time`.

Blocked on: M486 - Channel Network Core and M482 - Geometry VOP Node, for the kernel library and code generator.

Acceptance sketch:
- Lag and Spring on a step channel match closed-form references.
- `ChannelWrangle` `@V *= 2` matches a Math multiply exactly.
- Layer Mix with weights 0 and 1 returns each input exactly.

## Scope

**Out:**
- Sources beyond the generators: M489 - Channel Sources From Shapes, Tracks And Cameras, M490 - Channel Sources From Images And Audio.

**Assumptions:**
- Unconfirmed: the full list is checked against the Houdini CHOP docs in `.local/docs/houdini/nodes/chop/` when this is elaborated, and the docs never name Houdini.
