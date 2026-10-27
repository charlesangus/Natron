# USDMaterial

Authors a UsdPreviewSurface material on its scene input and binds it to the prims
that Prim Pattern names. The material prim is at Prim Path, which follows the
node's script name (`/Material1` for a node named Material1) until it is set by
hand. Under it the node authors a `PreviewSurface` shader whose `surface` output
is the material's surface.

Diffuse Color, Metallic, Roughness and Emissive Color set the shader's
`diffuseColor`, `metallic`, `roughness` and `emissiveColor` inputs. Colours are in
the project working space and are written unchanged: the node does no colour
conversion, and values outside 0 to 1 are kept. Animated knobs are written as
time samples.

Each input has a texture file knob. A texture that is set replaces the input's
value with a `UsdUVTexture` shader named after the input (`diffuseColorTexture`,
for example), read at the prims' `st` primvar through one shared
`UsdPrimvarReader_float2` named `TexCoordReader`. Colour textures feed the input
from their `rgb` output; Metallic and Roughness textures feed it from `r` and
are read as raw values. The file path is authored as written, even when it does
not resolve; the node then sets a warning that names each such path, and the
renderer draws the surface without that texture.

Prim Pattern is a list of absolute prim paths separated by spaces, whose names
may use `*` (any run of characters) and `?` (any one character). It is matched on
the input scene, and each match gets a `material:binding` to the material, which
applies to its descendants too unless they have their own. Prims under the
material itself are never bound. A pattern that matches nothing still authors
the material, unbound, and sets a warning naming the pattern; an empty pattern
authors the material unbound without a warning.

Image inputs and MaterialX are not supported.

Tolerance: none recorded.
