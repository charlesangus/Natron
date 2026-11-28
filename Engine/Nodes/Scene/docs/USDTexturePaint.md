# USDTexturePaint

Paints on the meshes of its scene input in the Viewer's 3D view, with the
brushes of RotoPaint, into one UDIM texture set. Prim Pattern names the meshes
(a USD path expression; the default `//*` takes every mesh of the scene, and
prims that are not meshes are ignored). All the matched meshes share the one
texture set, laid out by the UV Primvar (`st` by default): tile 1001 covers 0
to 1 in u and v, each tile further in u adds 1 and each row up in v adds 10.
Their UV layouts should not overlap.

The strokes painted through one camera form a session. Each session is a layer
under the node's base layer, and carries its camera (Camera To World, View
Height, Aspect, Orthographic, Near Clip and Far Clip), its Paint Model, its
Footprint and whether it is Frozen. Sessions and their strokes are saved with
the project, and sessions are laid over one another in the order they were
painted, whatever their models.

A session reaches the texture through one of two paint models:

- Buffer: strokes are drawn into a paint buffer through the session's camera,
  then projected into the tiles with occlusion when the session closes, so a
  stroke only lands on the surfaces the camera sees.
- Direct: every dab lands straight in the texels of the meshes under it while
  it is painted, so the texture updates live as the pen moves and there is
  nothing to project when the session closes. Dabs only land on surfaces the
  camera sees, and wrap across UV seams and onto every UDIM tile they reach.

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
- Footprint: where a dab painted straight into the texture lands. Surface (the
  default) paints the surface within the dab's radius of the point under its
  centre, measured on the mesh, so a dab keeps its size on a surface seen at a
  grazing angle. View paints exactly what the dab's disc covers on screen. Each
  session keeps the footprint it was started under.
- Paint Model: how the strokes of a new session reach the texture, Buffer
  (the default) or Direct, as described above. Each session keeps the model it
  was started under; changing it closes the open session.
- Output Mode: Over Base (the default) paints over the look of the meshes and
  outputs the painted texture. Paint Only paints over transparent black: every
  session's view is transparent, so the output's alpha is the strokes' coverage
  and its colour is premultiplied by it, ready to be layered over a material as
  `original * (1 - alpha) + paint`. Brushes that read the texture under them
  (Blur, Smear, Clone, Dodge, Burn) see nothing there in this mode. A Bake Output
  in Paint Only writes the paint alone beside the Bake Output Path files, with
  `paint` before the tile token (`usdTexturePaint_output.paint.<UDIM>.exr`), and
  the Bake Output Path files as that paint laid over the Bake Inputs base. The
  sessions it freezes live only in the files of the mode they were baked in, so
  Output Mode is locked while a Bake Output is in effect: undo the bake to change
  it.
- Bake Inputs Path: the files the input scene's look is baked into, one per tile,
  with a `<UDIM>` token for the tile number. They are the base strokes are
  painted over. The default is `[Project]/textures/usdTexturePaint_inputs.<UDIM>.exr`.
- Bake Output Path: the files the painted texture is baked into, one per tile,
  with a `<UDIM>` token. A bake renders and freezes the sessions there are when
  it starts, not those there were when it was queued; strokes painted after it
  starts go into a new session, which stays live for the next bake. If a session
  it rendered is changed before it finishes, it fails and puts its files back.
  The default is `[Project]/textures/usdTexturePaint_output.<UDIM>.exr`. A bake
  that does not finish puts back the files it replaced. A Bake Output fails, naming the other
  node, while another USDTexturePaint node's Bake Output of the same files has not
  finished. An undo or redo of a Bake Output waits for the node's queued Bake
  Outputs, and is refused, changing nothing, once a later one has landed or
  while another node is baking the same files.
- Output Tile: the UDIM tile the node outputs as its image (1001 by default).

The node does not yet render: its output is transparent black, the size of one
tile.

Tolerance: none recorded.
