# USDCamera

Authors a `UsdGeomCamera` at Prim Path (default `/` and the node's name, such as `/USDCamera1`, following renames until the path is edited). The node has no
input and is a scene source; combine it with other scenes through USDMerge, or put it
under a USDXformPrim.
The node does not touch colour.

Lens knobs are in millimetres and are written in USD's camera units, tenths of a
scene unit: a USD value is `mm / (100 x metersPerUnit)`, with metersPerUnit
0.01 (centimetres), so a 50 mm lens
is `focalLength = 50` in a centimetre scene and `0.5` in a metre scene. The camera
is not linked to the project format. Defaults are a 50 mm lens, a 36 x 24 mm
aperture and clipping 0.1 to 10000 scene units. F-Stop is written as is and Focus
Distance in scene units.

The transform is translate (double), rotate in the chosen order (degrees) and scale
(scale x Uniform Scale), authored as the prim's xformOpOrder.

Knobs that are not animated author default values. An animated knob (keyframes or
an expression) authors a time sample at every frame of the project range, at
timecode `frame x timeCodesPerSecond / project fps`; the layer's
time codes per second are the project frame rate.

A Prim Path that is not an absolute prim path sets a node error and the scene
action fails.

Tolerance: none recorded.
