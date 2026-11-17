# M484 - Image Kernel Binding And Wrangle

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

A per-pixel context for M360 - Geometry Kernel Language and a text `ImageWrangle` node, on CPU and GPU. The image binding implements `KernelBinding`: layer channels as `@attr`s (`@rgba`, `@Cd`, or any layer by name), `@P`/`@ix`/`@iy`/`@res`, `@Frame`/`@Time`, sampling of the input and second inputs at any coordinate with Natron's filters, and writes to output layers. The GPU half lowers through M408 - Kernel Language GPU Backend's Slang codegen, so images stay resident. This milestone is the base for M485 - Image VOP Node.

Blocked on: M408 - Kernel Language GPU Backend (the lowering and runner shape the GPU half extends) and F15 - Native Geometry landing M360 - Geometry Kernel Language on the default branch.

Acceptance sketch:
- `@rgba *= ch("gain")` on a 4K plate matches a Grade node's gain within ulps, on CPU and GPU.
- Neighbour sampling (`sample(0, @P + offset)`) gives a correct box blur, and the RoI grows by the declared sample radius.
- A snippet with a builtin that has no GPU form falls back to the CPU with a warning, as M408 does.

## Scope

**In:**
- An image `KernelBinding`, the `ImageWrangle` node, and CPU and GPU runners on render tiles.
- Layer and channel binding by M39 - Layers Not Planes names.

**Out:**
- The graph front end: M485 - Image VOP Node.
- Deep and volume wrangles: none.

**Edge cases:**
- A sample radius that isn't declared: the RoI is the output rect, and out-of-RoI reads return the input's edge mode.

**Assumptions:**
- Unconfirmed: users declare the sample radius with a knob (default 0) instead of the binding inferring it from the code.
