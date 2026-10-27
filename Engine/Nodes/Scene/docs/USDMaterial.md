# USDMaterial

Authors a UsdPreviewSurface material, as a scene source with no scene input. The
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

The node has five optional image inputs: Diffuse Color, Roughness, Metallic,
Normal and Emissive Color. A connected input drives the shader input of the same
name live from the image of the node connected to it, with no file on disk, and
wins over that input's texture file. Its `UsdUVTexture` reads the file
`natron://<node>/rgba?v=<hash>`, where node is the connected node's fully
qualified name and hash identifies its image. An image that does not change over
time has one path, written as the default. One that does gets a path per project
frame, written as time samples, each with its own hash. Diffuse Color and
Emissive Color read `rgb` with the colour space left to the renderer; Roughness and
Metallic read `r` as raw values. Normal takes a tangent-space normal map encoded
in 0 to 1. It is read as raw `rgb` with a scale of 2 and a bias of -1, and drives
the shader's `normal`, which the node writes only when the input is connected.

The material only names the images. The viewer and USDRender render them before
they draw a frame, and USD reads them through Natron's `natron` asset resolver.
Changing the connected image re-writes this node's material and no other layer.
An image that cannot be rendered leaves the renderer's fallback texture, and the
node shows a warning that names it.

MaterialX is not supported.

Tolerance: none recorded.
