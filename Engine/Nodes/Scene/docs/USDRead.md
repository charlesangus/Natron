# USDRead

Reads a USD file (`.usd`, `.usda`, `.usdc`, `.usdz`) or an Alembic file (`.abc`,
through USD's Alembic file-format plug-in) as a scene. Its layer sublayers the
file, so prims keep their paths, and carries the file's up axis, meters per unit,
default prim and time codes per second. Nothing is converted.

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

Tolerance: none recorded.
