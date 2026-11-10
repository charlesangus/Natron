# USDEmissiveMaterial

Authors an unlit UsdPreviewSurface material, as a scene source with no scene
input, for images shown on cards and projections. The surface gives off its
emission and nothing else: whatever the lights, and with none at all, it shows
the image as it is. The material prim is at Prim Path, which follows the node's
script name (`/Plate` for a node named Plate) until it is set by hand. Under it the node authors a `PreviewSurface` shader whose
`surface` output is the material's surface.

To use the material, connect the node to the Material input of a geometry node,
as for a USDMaterial. The geometry node places the material under its own prim
and binds itself to it with `material:binding`.

The shader's `diffuseColor` is black, its `metallic` 0 and `roughness` 1, and it
uses the specular workflow with a black `specularColor`, so it reflects no light.
Its `emissiveColor` is, in order of precedence:

- The image connected to the one input, Emissive, live from the node connected
  to it with no file on disk. Its `UsdUVTexture`, named `emissiveColorTexture`,
  reads `natron://<node>/rgba?v=<hash>`, with one path for an image that does
  not change over time and a path per project frame, written as time samples,
  for one that does.
- The Emissive Texture file, authored as written even when it does not resolve;
  the node then sets a warning that names the path.
- Emissive Color alone.

A texture is read at the bound prims' `st` primvar through a
`UsdPrimvarReader_float2` named `TexCoordReader`, with the colour space left to
the renderer, and its `rgb` feeds the emission. Emissive Color, white by
default, multiplies it through the texture's `scale`. Colours are in the
project working space and are written unchanged. Animated knobs are written as
time samples.

With Use Alpha on, the default, the alpha of the image on the Emissive input
drives the shader's `opacity` from the texture's `a` output, with `opacityMode`
set to `presence`: where the image is transparent the surface shows what is
behind it, at the ratio of the alpha, as the image composited over it. Live
textures are served with their colour divided by their alpha, as USD expects of
a texture, and the opacity multiplies it back. Where the alpha is 0, up to 8
pixels from pixels with an alpha, the colour is that of the nearest of them, so
the texture's filtering blends no black into soft edges; farther out it is left
as rendered. A surface with an opacity is drawn
in Storm's translucent pass, which writes no depth or prim id. With Use Alpha
off, or for a texture file, no opacity is written and the surface is opaque.

The material only names the images. The viewer and USDRender render them before
they draw a frame. An image that cannot be rendered shows as magenta, and
the node shows a warning that names it. That includes an image whose node was
deleted, until something is connected to the input or the deletion is undone.
Disconnecting the input is not a deletion: the material goes back to its own colour.

Tolerance: none recorded.
