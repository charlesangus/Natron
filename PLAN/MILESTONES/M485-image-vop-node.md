# M485 - Image VOP Node

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

`ImageVOP` is the graph twin of `ImageWrangle`. It is a `NetworkBuilder` in the comp graph with Image inputs and an Image output, and it reuses M482 - Geometry VOP Node's kernel library and code generator on M484 - Image Kernel Binding And Wrangle's binding. Image-specific nodes cover layer Bind/Export by name, Sample Input (with filter and coordinate ports) and pixel globals.

Blocked on: M482 - Geometry VOP Node and M484 - Image Kernel Binding And Wrangle.

Acceptance sketch:
- An ImageVOP grade-and-noise network matches its ImageWrangle text twin on CPU and GPU.
- Its generated code is viewable, and errors land on nodes, as in GeoVOP.

## Scope

**Out:**
- New kernel operators: M482 - Geometry VOP Node.
