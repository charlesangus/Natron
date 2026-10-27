# USDScopePrim

Defines a `Scope`, a grouping with no transform, at Prim Path (default `/` and
the node's name, such as `/USDScopePrim1`, following renames until the path is
edited) and grafts the root prims of every input under it. Missing ancestors of a
nested path are defined as `Scope`s. With no input the node defines just its
prim. The node does not touch colour.

Inputs, Auto Rename, Kind, the node error for an input root at the node's own
path and the node warning for links a graft cannot carry work as in USDXformPrim.

A Prim Path that is not an absolute prim path sets a node error and the scene
action fails.

Tolerance: none recorded.
