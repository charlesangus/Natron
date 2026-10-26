# USDLight

Authors a UsdLux light at Prim Path (default `/` and the node's name, such as `/USDLight1`, following renames until the path is edited). Type picks the prim:
`DistantLight` (Angle), `SphereLight` (Radius), `RectLight` (Width, Height) or
`DomeLight` (Texture File). With a scene input the light is layered over the
input's scene; without one the node is a scene source.

Color, Intensity, Exposure and Normalize map directly to `inputs:color`,
`inputs:intensity`, `inputs:exposure` and `inputs:normalize` for every type.
Normalize is on by default, so Intensity alone sets the brightness whatever the
light's size: UsdLux scales an unnormalized light's power by its area, or for a
distant light by its solid angle, which leaves a 0.53 degree distant light of
Intensity 1 about 7e-5 as bright as a normalized one.
Color is in the project working space and written unchanged; the node does no
colour conversion.

A dome's Texture File is authored as `inputs:texture:file` whether or not it
resolves. When it does not resolve, the node sets a persistent warning naming the
path and the light renders untextured; the warning clears once the path resolves
or the type changes.

The transform knobs, animation and prim path rules are those of USDCamera.

Tolerance: none recorded.
