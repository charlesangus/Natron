# M483 - Particle VOP Node

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

`PopVOP` is the graph twin of `PopWrangle`. It reuses M482 - Geometry VOP Node's kernel node library and code generator on M406 - Particle Forces And Wrangle's particle binding, adding particle globals (age, life, `@v`, `@force`, `@TimeInc`) as Global node outputs. It takes the GPU path when M409 - Particle GPU Path keeps particles resident.

Blocked on: M482 - Geometry VOP Node and M406 - Particle Forces And Wrangle (the particle binding, and how PopWrangle sits in the solver).

Acceptance sketch:
- A PopVOP adding curl noise to `@force` matches the equivalent PopWrangle snippet.
- PopVOP runs inside the particle solver wherever PopWrangle can.

## Scope

**In:**
- The `PopVOP` node, particle globals and the particle-specific nodes the binding offers.

**Out:**
- New kernel operators: M482 - Geometry VOP Node owns the library.
