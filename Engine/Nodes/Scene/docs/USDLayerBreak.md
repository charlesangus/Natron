# USDLayerBreak

Marks everything in its scene input as context, so that a USDWrite downstream
writes only the edits made after the break. It has no controls, authors nothing
and does not change the scene: the viewer, renders and every node's queries see
the same stage with or without it.

Every layer upstream of the break is context. Context layers still compose, so
nodes after the break can select, edit and reference what they define, but a
USDWrite with Strip Layers Above Breaks on leaves them out of the file it writes.
A file a USDRead reads above the break is left out too, rather than sublayered by
path, so the written file is a pure set of edits that layers over any version of
the scene it was made against. It does not open on its own as a complete scene.

For example, a lighting artist reads `landscape.usd`, adds a USDLayerBreak, then
a USDPrune that deactivates a scratch light and a few USDLight nodes. A USDWrite
at the end writes `lighting.usd` holding only the `over` that deactivates the
scratch light and the new lights. The pipeline later layers `lighting.usd` over
`landscape.usd`. Prims that only the context defines, edited after the break,
are written as `over` specs, as in any layer that does not define them. Stage
metadata (up axis, meters per unit, default prim, time range, render settings)
is still written from the composed scene.

Through a USDMerge, a layer is context only when every branch reaching the merge
carries it through a break. A branch without a break is written whole, and a
layer it shares with a broken branch, as when one USDRead feeds both, is written
too, since that branch relies on it.

Breaks are cumulative: a second break marks the layers between the two as
context as well. A disabled break passes its input through, marking nothing.
Breaks inside the scene a USDReference references, or the scenes a
USDXformPrim grafts under itself, are ignored: those scenes are written whole.

Tolerance: none recorded.
