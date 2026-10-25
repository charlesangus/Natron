# USDWrite

Writes its scene input to one USD file: `.usda` for text, `.usdc` for binary, and
`.usd` for binary under the name every USD application opens. Like Write it is
an output node: the Render button, the render queue and `NatronRenderer -w`
run it. Whatever the frame range, a render writes the file once, holding the
time samples of the whole range. An existing file is overwritten. The node's
output is its input, unchanged.

Frame Range sets the frames written: the project's range (the default) or First
Frame to Last Frame. Time samples outside it are dropped, after samples are added
at its ends with the value the animation had there, so the animation over the
range is unchanged. The file's start and end time codes are the range's, and its
time codes per second, up axis, meters per unit and default prim are the
scene's.

Export Mode sets what goes into the file:

- Layered (the default) flattens every layer the project's nodes authored into
  the file, and keeps the USD files the scene reads (USDRead's) as sublayers
  with the time offsets they were read with, as Solaris's USD ROP does with
  Flatten Implicit Layers. Those files are referred to, not copied, and their
  own time samples are not clipped. What the nodes author is stronger than
  every file read, so an edit authored below a USDRead in the graph that the
  file overrides comes out on top.
- Flattened bakes the whole composed scene, the files it reads included, into
  the one file, which then stands on its own.

Asset Paths sets how the file refers to other files (the sublayers of Layered
mode, references, textures and other asset-valued attributes): relative to the
written file's directory (the default), with `./` in front unless they start
with `..`, or absolute. A path that cannot be made relative is written absolute.
URIs a resolver handles are kept as they are.

The node sets an error, and the render fails, when File is empty or does not end
in a USD extension, when the input scene fails, or when Layered mode would
overwrite a file the scene reads.

Tolerance: none recorded.
