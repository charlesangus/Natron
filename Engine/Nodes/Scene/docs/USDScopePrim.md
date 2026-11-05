# USDScopePrim

Defines a `Scope`, a grouping with no transform, at Prim Path (default `/` and
the node's name, such as `/USDScopePrim1`, following renames until the path is
edited) and grafts the root prims of every input under it. Missing ancestors of a
nested path are defined as `Scope`s. With no input the node defines just its
prim. The node does not touch colour.

Inputs, Auto Rename, Kind, the node error for an input root at the node's own
path, the node warning for links a graft cannot carry and the Materials Scope
pass-through work as in USDXformPrim: an input root named like the project's
Materials Scope stays at its own path. A USDScopePrim whose Prim Path is the
scope itself, such as `/materials`, gathers materials into it; an input root
`/materials` passes through as that same prim rather than nesting at
`/materials/materials`.

A Prim Path that is not an absolute prim path sets a node error and the scene
action fails.

Tolerance: none recorded.
