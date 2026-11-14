# TexturePaint

Paints on the meshes of its scene input in the Viewer's 3D view, with the
brushes of RotoPaint, into one UDIM texture set. Prim Pattern names the meshes
(a USD path expression; the default `//*` takes every mesh of the scene, and
prims that are not meshes are ignored). All the matched meshes share the one
texture set, laid out by the UV Primvar (`st` by default): tile 1001 covers 0
to 1 in u and v, each tile further in u adds 1 and each row up in v adds 10.
Their UV layouts should not overlap.

Strokes are drawn into a paint buffer through the view camera they are painted
through, then projected into the tiles with occlusion, so a stroke only lands on
the surfaces the camera sees. The strokes painted through one camera form a
session. Each session is a layer under the node's base layer, and carries its
camera (Camera To World, View Height, Aspect, Orthographic, Near Clip and Far
Clip) and whether it is Frozen. Sessions and their strokes are saved with the
project.

Controls:

- Prim Pattern: the meshes to paint on.
- UV Primvar: the texture coordinate primvar the tiles are laid out by.
- Tile Resolution: the width and height of each UDIM tile, 1K to 8K (4K by
  default).
- Buffer Resolution: the longer side of the paint buffer, 1K to 8K (2K by
  default), whatever the size of the Viewer; the shorter side follows the view's
  aspect. 2K keeps a full-HD view at or above one buffer pixel per screen pixel
  while bounding the buffer's memory and projection cost. A larger buffer keeps
  finer strokes when zoomed in close.
- Texture Mode: Static paints one texture set for every frame; Animated keeps a
  texture set per frame.
- Bake Inputs Path: the files the input scene's look is baked into, one per tile,
  with a `<UDIM>` token for the tile number. They are the base strokes are
  painted over. The default is `[Project]/textures/texturePaint_inputs.<UDIM>.exr`.
- Bake Output Path: the files the painted texture is baked into, one per tile,
  with a `<UDIM>` token. A bake renders and freezes the sessions there are when
  it starts, not those there were when it was queued; strokes painted after it
  starts go into a new session, which stays live for the next bake. If a session
  it rendered is changed before it finishes, it fails and puts its files back.
  The default is `[Project]/textures/texturePaint_output.<UDIM>.exr`. A bake
  that does not finish puts back the files it replaced. A Bake Output fails, naming the other
  node, while another TexturePaint node's Bake Output of the same files has not
  finished. An undo or redo of a Bake Output waits for the node's queued Bake
  Outputs, and is refused, changing nothing, once a later one has landed or
  while another node is baking the same files.
- Output Tile: the UDIM tile the node outputs as its image (1001 by default).

The node does not yet render: its output is transparent black, the size of one
tile.

Tolerance: none recorded.
