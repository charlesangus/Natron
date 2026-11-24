# USDLayerBegin

Opens a layer region. Every layer the nodes after it add goes into the region,
until the matching USDLayerEnd, a USDMerge, a USDLayerBreak or the USDWrite.
USDWrite writes each region as one layer, which the file of the region around it
sublayers. It has no controls, authors nothing and does not change the scene:
the viewer, renders and every node's queries see the same stage with or without
it.

A region opened by USDLayerBegin names no file. A USDConfigureLayer inside it, in
either mode, sets its Save Path and layer metadata. A region nobody configures
is written into the file of the region around it, or the USDWrite's own output.

Regions nest: a second USDLayerBegin, or a USDConfigureLayer in Following Layers
mode, opens a region inside the current one, written as a sublayer of it. A
USDLayerEnd closes the innermost region.

A region still open at a USDMerge closes at that input, so another branch's
layers never land in its file. A file a USDRead reads inside a region is
sublayered by that region's file. A region above a USDLayerBreak is context, and
nothing of it is written.

Tolerance: none recorded.
