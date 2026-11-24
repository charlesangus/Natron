# USDLayerEnd

Closes the innermost layer region, opened by a USDLayerBegin or by a
USDConfigureLayer in Following Layers mode. The layers the nodes after it add go
into the region around it, or into no region. It has no controls, authors
nothing and does not change the scene.

With no region open on its input, as when a USDMerge, a USDLayerBreak or another
USDLayerEnd has already closed it, the node passes its input through and shows a
warning.

Tolerance: none recorded.
