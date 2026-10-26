# USDRead

Reads a USD file (`.usd`, `.usda`, `.usdc`, `.usdz`) or an Alembic file (`.abc`,
through USD's Alembic file-format plug-in) as a scene. Its layer sublayers the
file, so prims keep their paths, and carries the file's up axis, meters per unit,
default prim and time codes per second. Nothing is converted.

Time maps by seconds: project frame `f` reads the file at timecode
`(f + Frame Offset) x timeCodesPerSecond / project fps`, identical to `f + Frame
Offset` when the rates match.

Reload reads the file again and re-keys the node. The file is read into a new
layer rather than reloaded in place, because stages on render threads may be
reading the old one; a file whose stamp moved (Reload, clearing the caches) is
likewise opened anew on the next evaluation. Files it sublayers, references or
loads as a payload are USD's shared layers and are only read again once no
stage, shown or cached, keeps them open. A file that is missing or does not parse sets a
node error carrying USD's diagnostic, and the scene action fails.

Tolerance: none recorded.
