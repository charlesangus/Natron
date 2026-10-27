# USDXformPrim

Defines an `Xform` at Prim Path (default `/` and the node's name, such as
`/USDXformPrim1`, following renames until the path is edited) and grafts the root
prims of every input under it, so the node graph's shape is the scene's: a Card1
source into USDXformPrim "Transform1" gives `/Transform1/Card1`. Missing ancestors
of a nested path, such as `/set` of `/set/Transform1`, are defined as `Xform`s.
With no input the node defines just its prim. The node does not touch colour.

Inputs, numbered 0 to 63, graft in input order. Where two inputs have a root prim
of the same name the later input is stronger and the two compose, as in USDMerge;
with Auto Rename on, the later one takes the first free `_1`, `_2`, ... suffix
instead (`/Transform1/Card1_1`). An input root at the node's own path, which would
be grafted inside itself, is a node error.

A relationship target or attribute connection that leaves its root prim follows
the graft when its target lies under a root the same input grafts. One that lies
under none is dropped from the grafted property and named in a node warning,
which clears once no such link remains.

An input root named like the project's Materials Scope (Project Settings,
default `/materials`) is not grafted: it stays at its own path, so materials
that USDAssignMaterial placed there are shared by every prim bound to them
rather than copied under each transform. Links into the scope, such as a
material binding, keep pointing at it. A scope root that holds geometry still
stays put, with a node warning.

Kind sets the prim's model kind (group, assembly, component or subcomponent);
None authors no kind.

The transform is translate (double), rotate in the chosen order (degrees) and
scale (scale x Uniform Scale) about Pivot, authored as one `xformOp:transform`
matrix op, and an `xformOpOrder` holding only it, on the prim. The transform and
kind sit in a layer of their own that authors only `over`s, so editing them does
not recompose the grafted prims.

Knobs that are not animated author default values; an animated knob authors a
time sample at every frame of the project range, as in USDCamera.

A Prim Path that is not an absolute prim path sets a node error and the scene
action fails.

Tolerance: none recorded.
