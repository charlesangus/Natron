# USDMaterial

Authors a UsdPreviewSurface material, as a scene source with no input. The
material prim is at Prim Path, which follows the node's script name
(`/Material1` for a node named Material1) until it is set by hand. Under it the
node authors a `PreviewSurface` shader whose `surface` output is the material's
surface.

To use the material, connect the node to the Material input of a geometry node.
The geometry node places the material under its own prim (`/Card1/Material1`)
and binds itself to it with `material:binding`, which applies to the geometry's
descendants too unless they have their own. The material moves with the
geometry: under a USDXformPrim `/Transform1` the card binds
`/Transform1/Card1/Material1`. One material node feeding two geometry nodes gives
each its own copy.

Diffuse Color, Metallic, Roughness and Emissive Color set the shader's
`diffuseColor`, `metallic`, `roughness` and `emissiveColor` inputs. Colours are in
the project working space and are written unchanged: the node does no colour
conversion, and values outside 0 to 1 are kept. Animated knobs are written as
time samples.

Each input has a texture file knob. A texture that is set replaces the input's
value with a `UsdUVTexture` shader named after the input (`diffuseColorTexture`,
for example), read at the bound prims' `st` primvar through one shared
`UsdPrimvarReader_float2` named `TexCoordReader`. Colour textures feed the input
from their `rgb` output; Metallic and Roughness textures feed it from `r` and
are read as raw values. The file path is authored as written, even when it does
not resolve; the node then sets a warning that names each such path, and the
renderer draws the surface without that texture.

Image inputs and MaterialX are not supported.

Tolerance: none recorded.
