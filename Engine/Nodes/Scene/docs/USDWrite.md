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
  own time samples are not clipped. Every layer keeps its strength: what the
  nodes authored above the strongest file goes into the written file, and what
  they authored below a file goes, flattened, into a file of its own beside it
  (`name_layer1.usda`, `name_layer2.usda`, ... for `name.usda`), sublayered in
  its place. A file the scene reads above an authored edit still overrides it.
- Flattened bakes the whole composed scene, the files it reads included, into
  the one file, which then stands on its own.

Asset Paths sets how the file refers to other files (the sublayers of Layered
mode, references, textures and other asset-valued attributes): relative to the
written file's directory (the default), with `./` in front unless they start
with `..`, or absolute. A path that cannot be made relative is written absolute.
URIs a resolver handles are kept as they are. The nodes author the paths their
file knobs name made absolute (a relative one against the project's directory,
or the working directory for an unsaved project), so a relative path in the
written file resolves from the file's own folder.

Export Live Textures (on by default) writes the images USDMaterial's image
inputs feed the scene (its `natron://` live textures, which only the running
Natron session can read) to OpenEXR files in a `name_textures` folder beside
`name.usda`, and points the texture paths at them, following Asset Paths. Each
is `<node>_<layer>.exr` for an image that does not change over time, and
`<node>_<layer>.####.exr`, one per frame of the range with its frame number, for
one that does, the path time-sampled per frame. The files are half-float RGBA,
ZIP-compressed. Off, the `natron://` paths are written as they are, and the node
warns that the file is not portable.

A layer already open on a written file, such as a USDRead of it, is reloaded in
place, so every viewer and node reading it shows the new content.

The node sets an error, and the render fails, when File is empty or does not end
in a USD extension, when the input scene fails, when Layered mode would
overwrite a file the scene reads, or when a live texture cannot be rendered or
written.

Tolerance: none recorded.
