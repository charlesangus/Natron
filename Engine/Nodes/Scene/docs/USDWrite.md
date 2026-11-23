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
  with the time offsets they were read with. Those files are referred to, not
  copied, and their own time samples are not clipped. Every layer keeps its
  strength: what the nodes authored above the strongest file goes into the
  written file, and what they authored below a file goes, flattened, into a file
  of its own beside it (`name_layer1.usda`, `name_layer2.usda`, ... for
  `name.usda`), sublayered in its place. A file the scene reads above an
  authored edit still overrides it. The layer regions given a Save Path go to
  files of their own, described below.
- Flatten Layers flattens the layer stack, the files the scene reads included,
  into the one file without composing it: references, payloads, variant sets,
  inherits and list edits stay as they are authored, so variants can still be
  switched in the written file. A reference or payload to a USD file on disk
  stays one, following Asset Paths. The scenes the nodes reference (a
  USDReference of its input, or a USDXformPrim's grafted scene) are flattened
  the same way and copied into the file under `/_references/stack1`,
  `/_references/stack2`, ..., a `class` that draws nothing itself, and the
  arcs to them point there, so the file stands on its own. Layer regions are
  not written as files: their metadata goes to the one file.
- Flattened bakes the whole composed scene, the files it reads included, into
  the one file, which then stands on its own.

In Layered mode a layer region (see USDLayerBegin, USDLayerEnd and
USDConfigureLayer) whose Save Path is set is written to that file, with the
layers it holds flattened into it as the written file's are, and sublayered
where its layers were: a region nested in another goes into the outer region's
file, and a region opened after a USDRead that gathers the layers before it
(Applies To set to Preceding Layers) sublayers that read file too. A Save Path
is resolved against the written file's folder, with the project's path
variables and the first frame and view of the range, as File is, and its
folder is created. It must end in a USD extension, differ from File, and not
name a file the scene reads. The region's metadata (default prim, comment,
start and end time, time codes and frames per second, up axis, meters per unit,
colour configuration and system, render settings prim and custom data) goes on
its file, each field only when its node authors it. A region with no Save Path
stays in the file that holds it, and its metadata goes on that file, the
written file's metadata included. A region file's time codes per second are the
scene's: composition scales its layers as it did when the scene composed them.

Save Collisions sets what happens when two regions resolve to the same Save
Path: Error (the default) fails the render, naming both nodes, before any file
is written. Merge Into One File writes the regions' layers to the one file in
the order they are stacked, the stronger region's metadata winning field by
field. A region whose layers are interleaved with another region's in the
layer stack, which happens when the layers a Preceding Layers configuration
gathers after a USDMerge span a region of one branch, takes the layers between
its own into its file, so every layer keeps its strength; two regions that
each hold part of the other fail the render, naming both nodes.

Strip Layers Above Breaks (on by default) leaves out, in every mode, every
layer above a USDLayerBreak (the context layers), the files a USDRead above
the break reads and the layer regions above it, which write no file, so the file holds only the edits made after the break and
layers over the scene it was made against. The file is not sublayered by path:
it does not open on its own as a complete scene. Prims only the context defines,
edited after the break, are written as `over` specs. In Layered mode the scenes
a USDReference after the break references are still written as files beside the
written one. In Flattened mode the layers after the break are merged into one
layer, keeping list edits, variant selections, arcs and deactivations as they
are authored; the scenes a USDReference after the break references are baked in.
The stage metadata is the whole scene's either way. With no break upstream, or with the toggle off, the whole
scene is written.

Asset Paths sets how the file refers to other files (the sublayers of Layered
mode, references, textures and other asset-valued attributes): relative to the
written file's directory (the default), with `./` in front unless they start
with `..`, or absolute. A path that cannot be made relative is written absolute.
URIs a resolver handles are kept as they are. The nodes author the paths their
file knobs name made absolute (a relative one against the project's directory,
or the working directory for an unsaved project), so a relative path in the
written file resolves from the file's own folder.

Export Live Textures (on by default) writes the images USDPreviewSurface's image
inputs feed the scene (its `natron://` live textures, which only the running
Natron session can read) to OpenEXR files in a `name_textures` folder beside
`name.usda`, and points the texture paths at them, following Asset Paths. Each
is `<node>_<layer>.exr` for an image that does not change over time, and
`<node>_<layer>.####.exr`, one per frame of the range with its frame number, for
one that does, the path time-sampled per frame. A texture spans the image's
format and its data beyond it, overscan included. The files are half-float RGBA, ZIP-compressed, with straight colour rather than
premultiplied, of the project's first view. They are written before the USD files,
so a texture that fails leaves the earlier USD files as they were. Off, the
`natron://` paths are written as they are, and the node warns that the file is not
portable. A `natron://` path whose source is no longer available, such as one read
from an earlier file, is written as it is with a warning saying so. A texture
whose node was deleted is left out, whether or not live textures are exported:
the input it fed keeps the material's own value, such as its Diffuse Color, and
the node warns, naming the material.

A layer already open on a written file, such as a USDRead of it, is reloaded in
place, so every viewer and node reading it shows the new content.

The node sets an error, and the render fails, when File is empty or does not end
in a USD extension, when the input scene fails, when Layered mode would
overwrite a file the scene reads, when a write that strips breaks would
overwrite a file read above a break, when a region's Save Path is not a USD
file, is File, is read by the scene or collides under Error, or when a live
texture whose node exists cannot be rendered or written.

Tolerance: none recorded.
