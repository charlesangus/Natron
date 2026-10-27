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

A file that is missing or does not parse sets a
node error carrying USD's diagnostic, and the scene action fails.

Tolerance: none recorded.
