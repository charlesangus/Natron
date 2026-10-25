# USDLight

Authors a UsdLux light at Prim Path (default `/Light`). Type picks the prim:
`DistantLight` (Angle), `SphereLight` (Radius), `RectLight` (Width, Height) or
`DomeLight` (Texture File). With a scene input the light is layered over the
input's scene; without one the node is a scene source.

Color, Intensity, Exposure and Normalize map directly to `inputs:color`,
`inputs:intensity`, `inputs:exposure` and `inputs:normalize` for every type.
Color is in the project working space and written unchanged; the node does no
colour conversion.

A dome's Texture File is authored as `inputs:texture:file` whether or not it
resolves. When it does not resolve, the node sets a persistent warning naming the
path and the light renders untextured; the warning clears once the path resolves
or the type changes.

The transform knobs, animation and prim path rules are those of USDCamera.

Tolerance: none recorded.
