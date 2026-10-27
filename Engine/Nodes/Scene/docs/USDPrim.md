# USDPrim

Authors one prim for layout at Prim Path (default `/` and the node's name, such as `/USDPrim1`, following renames until the path is edited): an `Xform` with a
transform, or a `Scope` that only groups. With a scene input the prim is layered
over the input's scene; without one the node is a scene source. The node does not
touch colour.

Kind sets the prim's model kind (group, assembly, component or subcomponent)
through `UsdModelAPI`; None authors no kind.

An Xform's transform is translate (double), rotate in the chosen order (degrees)
and scale (scale x Uniform Scale), authored as one xformOpOrder that replaces
whatever the input authors on the same prim. A Scope hides the transform knobs and
authors no xform ops.

Knobs that are not animated author default values; an animated knob authors a
time sample at every frame of the project range, as in USDCamera.

A Prim Path that is not an absolute prim path sets a node error and the scene
action fails.

Tolerance: none recorded.
