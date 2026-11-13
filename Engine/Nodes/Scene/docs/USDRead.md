# USDRead

Reads a USD file (`.usd`, `.usda`, `.usdc`, `.usdz`) or an Alembic file (`.abc`,
through USD's Alembic file-format plug-in) as a scene. Its layer sublayers the
file, so prims keep their paths, and carries the file's up axis, meters per unit,
default prim and time codes per second. Nothing is converted unless a Match
toggle below is on.

Time maps by seconds: project frame `f` reads the file at timecode
`(f + Frame Offset) x timeCodesPerSecond / project fps`, identical to `f + Frame
Offset` when the rates match.

The file is opened once as USD's shared layer for it, which every reader of the
file and every stage composing it use. A file changed on disk is read again only
by Reload: it reloads, in place, the file and every file it sublayers, references
or loads as a payload whose modification time moved, so every stage using them,
the viewer's included, shows the new content. A reload waits for the scene
evaluations under way to finish and stops the viewers' renders first
(SceneWriteBarrier). The node is keyed on the stamps of all those files as of its
last Reload, so a changed nested file re-authors its layer, and the other nodes
reading the same file are re-keyed with it.

A file that is missing or does not parse sets a
node error carrying USD's diagnostic, and the scene action fails.

## Up axis and units

The project's Up Axis and Meters Per Unit are what every Natron-authored layer
uses. A file whose metrics differ is read as it is, with a node warning naming
each aspect that differs and the toggle that converts it; a metric the file does
not author is named as USD's fallback (`Y`, `0.01`).

- **Match Project Up Axis** rotates a Z-up file -90 degrees about X in a Y-up
  project, and a Y-up file +90 degrees in a Z-up project.
- **Match Project Units** scales the file uniformly by its meters per unit / the
  project's. Camera clipping ranges, focus distances, focal lengths and apertures
  (time samples included) are rescaled too, since the transform does not reach
  them; other lengths (light sizes, shader parameters) are not.

Both default off and correct independently. The correction is a `rotateX` and/or
`scale` op prepended to the transform of each topmost transformable prim (after
`!resetXformStack!` where there is one, and on any descendant that resets the
stack), authored in this node's layer: the file is not edited and no prim moves
in the hierarchy, so downstream prim patterns still match. A prim that already
has an op of the same name, such as a file exported from a corrected read, gets
the next `_<n>` name. Once an aspect is converted the layer carries the project's
value for it, so the scene reports the project's metrics from this node on.

**Use File's Metrics For Project** sets the project's Up Axis and Meters Per Unit
to the file's (fallbacks where unauthored), as one undoable edit. It is a one-shot
copy, and is disabled while no file is set.

## Initial Draw

**Initial Draw** sets how viewers draw the file's top-level prims: the prims that
the file, or a file it sublayers, defines at its root. A root prim the file only
overs is left alone, and so are prims other nodes add to the scene, such as a
primitive merged with the read, which keep their own draw.

- **BBox** (default) draws each top-level prim as one box, from its extents hint,
  or as a marker where it has none. A large file shows its layout at once, and no
  payload is loaded.
- **Levels** opens the hierarchy **Levels** deep below each top-level prim and
  boxes the prims there; 1 boxes the top-level prims' children.
- **To Kind** opens the hierarchy below each top-level prim down to the first
  prim whose kind is **To Kind** or derives from it (an Assembly is a Group;
  Groups and Components are Models), and boxes that prim.
- **Full** draws the top-level prims in full, as far as their payloads are
  loaded.
- **Full + Load** draws them in full and has each viewer load every payload
  below them.

Both walks stop at an unloaded payload, which is boxed where it is. These are
draw rules carried beside the scene, not edits to it: the node's layer is the
same whatever Initial Draw says, renders and written files never see them, and a
change re-draws the viewers without composing anything again. A draw state set in
a viewer, or set by a node further down the graph, takes precedence. Rows the
rules decide name this node as their origin.

Tolerance: none recorded.
