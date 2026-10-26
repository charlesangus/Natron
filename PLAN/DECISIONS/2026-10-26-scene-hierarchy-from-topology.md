# Scene hierarchy follows graph topology

2026-10-26 — user, in the M19/M20 UAT grill.

By default, the 3D node graph's topology is the scene hierarchy. Card1 → Transform1 and
Sphere1 → Transform2, merged, compose as `/Transform1/Card1` and `/Transform2/Sphere1`. Nuke's
USD system builds a flat scene with mask-based edits instead, and the user called that
backwards. The node set changes as follows:

- **`USDXformPrim` / `USDScopePrim`** re-parent every root prim of their inputs under their
  own prim path, creating missing ancestors. They take any number of inputs, and an optional
  auto-rename knob controls collisions. With it off, colliding roots compose with later
  inputs stronger, as in USDMerge. `USDXformPrim` carries the transform. These replace
  `USDPrim`. There is no generic `USDAddPrim`; typed prims come from M52 - USD Layer-Stack
  Completion's schema-driven node.
- **`USDTransform` → `USDEditTransform`**, keeping its Solaris-style pattern-edit semantics for
  imported scenes.
- **Create nodes** (camera, light, primitives) have no scene input and author at
  `/<NodeName>`.
- **USDRead** keeps the file's own root paths. To nest a file, feed it into a USDXformPrim.
- **Up axis and meters per unit** are project settings that every created node authors. Files
  that disagree get a warning and are never converted.

This amends the node names in `2026-10-24-usd-node-naming.md`, whose `USD<Whatever>` rule
still holds. It is applied in M226 - Scene Hierarchy From Topology. Survey and context:
`PLAN/DESIGN/2026-10-26-3d-mvp-survey.md`.
