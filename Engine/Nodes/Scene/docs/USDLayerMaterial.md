# USDLayerMaterial

Lays paint over the materials already bound to the prims of its Scene input that
Prim Pattern names, live, without baking. Connect the paint to the Paint input,
typically a USDTexturePaint in Paint Only mode, whose planes are premultiplied by
their coverage. Each material input becomes `original * (1 - a) + paint`, where
`a` is the paint plane's alpha.

For every material bound to a matched gprim or geometry subset (under the
purpose Material Purpose names), the node authors `<material>_layered` beside it.
It specializes the original, so every edit made upstream to the original still
shows through. The matched geometry is bound to it with a collection binding on
its root prim, written as the strongest there. Prims the pattern leaves out keep
the original material, even where they share it with matched prims. A subset
bound to another material than its gprim is layered with its own material.

Which plane of the Paint input lies over which input comes from each input's
Layer menu:

- From Metadata (the default) takes the plane whose USDTexturePaint purpose is the
  input, such as the colour plane for `diffuseColor`. With no plane naming the
  input, the input is left as it is.
- None leaves the input as it is.
- A plane of the Paint input lays that plane over the input.

Two kinds of material are layered:

- UsdPreviewSurface: the layered material gains a Storm surface
  (`natronLayeredPreviewSurface.glslfx`) that takes the original surface's inputs
  and mixes a paint texture over each mapped input before lighting. Scalar inputs
  take the paint's first channel. A normal plane holds the painted normal
  premultiplied, without scale or bias. The universal surface stays the
  original's, which the other renderers read, so they show the material without
  the paint. MaterialBake, which Bake Inputs on a downstream USDTexturePaint uses,
  evaluates the paint over the original.
- A USDProject3D projection: the paint lies over the projected plate, from the
  Diffuse Color Layer plane.

A material that is already layered is layered again, so layers stack: bakes lay
every layer in order. The 3D viewer shows only the latest layer of an input that
two layers paint, and the node warns there.

The node warns, and leaves the material as it is, for MaterialX materials
("MaterialX layering is not implemented"), prims with no bound material, other
surfaces, and the prims of native instances. It also warns where a binding
stronger than its own keeps a matched prim on another material.

Each layer `k` stores its texture over input `<input>` as the shader
`Layer<k>_<input>` under the layered material, and the material's
`natron:layerDepth` attribute counts its layers.

Controls:

- Prim Pattern: the prims whose materials the paint lies over, with the geometry
  under them.
- Material Purpose: All, Full or Preview. The binding whose material is layered,
  and the purpose the binding to the layered material is written for.
- Mix: how much of the paint lies over the material, from 0 to 1. It scales the
  paint and its alpha together.
- Diffuse Color Layer, Emissive Color Layer, Specular Color Layer, Metallic Layer,
  Roughness Layer, Clearcoat Layer, Clearcoat Roughness Layer, Opacity Layer,
  Normal Layer, Occlusion Layer: the plane of the Paint input over each input.
