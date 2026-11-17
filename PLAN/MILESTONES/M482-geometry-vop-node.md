# M482 - Geometry VOP Node

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

The first kernel-target network. `GeoVOP` is a `NetworkBuilder` (M351 - Shader Graph Presentation) in the geometry graph. It is the graph twin of `GeoWrangle` from M360 - Geometry Kernel Language. The network generates kernel-language source and runs through the existing front end and runner on `GeoKernelBinding` (`PLAN/DECISIONS/2026-11-17-generic-port-networks.md`). This milestone also lands the context-neutral kernel node library and code generator, which M483 - Particle VOP Node, M485 - Image VOP Node and M488 - Channel Operators And Channel VOP reuse.

Blocked on: M349 - Shader Node Spike's design note (the network node class, the runtime spec, named outputs, port drawing) and F2 - Native Shader Networks landing the generic core in M350 - Shader Manifest And Nodes and M351 - Shader Graph Presentation. It also needs F15 - Native Geometry landed, since M360 - Geometry Kernel Language lives on that branch.

Acceptance sketch:
- A GeoVOP that binds `P`, adds noise scaled by a Parameter node and exports `P` matches the equivalent `GeoWrangle` snippet bit-for-bit.
- A wiring or type error shows on the offending node, and "View Code" shows the generated snippet with that node's lines highlighted.
- Parameter nodes appear on the GeoVOP's panel and can be animated.

## Scope

**In:**
- Kernel node specs in the generic manifest schema (M350 - Shader Manifest And Nodes), with `targets: [kernel]` and a code template per node.
- Generic operators: arithmetic, trig, vector and matrix ops, Compare, Two Way Switch, noise family, Ramp, Constant.
- Bind and Bind Export (read and write an `@attr`), Global (context globals from the binding, e.g. `@ptnum`, `@Frame`), and Parameter (a container knob read through `ch()`).
- Structure nodes: Snippet (inline kernel source), Subnet (nesting the same container class), If block and For/While loop blocks.
- A code generator: topological emission into kernel source, a line-to-node map, compile errors routed to nodes, and a "View Code" panel.
- The `GeoVOP` node: run over points, primitives, vertices or detail, as GeoWrangle does; second-input reads.

**Out:**
- Particle, image and channel contexts: M483 - Particle VOP Node, M485 - Image VOP Node, M488 - Channel Operators And Channel VOP.
- Lowering generic nodes into renderer shaders: never (decision 2026-11-17-generic-port-networks).
- The GPU path beyond what M408 - Kernel Language GPU Backend gives GeoWrangle: none. Generated source takes the same path.

**Edge cases:**
- Cycles in the wiring: refused by the existing cycle check.
- An unconnected required input: the node's default value is emitted, as Houdini does.
- A Snippet whose code fails to compile: the error lands on the Snippet node with its own line number.
- Two Bind Exports of one attribute: the later one in topological order wins, and the node warns.

**Assumptions:**
- Unconfirmed: the node names `GeoVOP` and `PopVOP`, following Houdini's attribute VOP naming without the word VEX.
- Codegen plus compile stays under about 1 ms per edit. M360's front end compiles in about 0.1 ms.
