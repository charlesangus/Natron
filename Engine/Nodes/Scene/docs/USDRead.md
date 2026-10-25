# USDRead

Reads a USD file (`.usd`, `.usda`, `.usdc`, `.usdz`) or an Alembic file (`.abc`,
through USD's Alembic file-format plug-in) as a scene. Its layer sublayers the
file, so prims keep their paths, and carries the file's up axis, meters per unit,
default prim and time codes per second. Nothing is converted.

Time maps by seconds: project frame `f` reads the file at timecode
`(f + Frame Offset) x timeCodesPerSecond / project fps`, identical to `f + Frame
Offset` when the rates match.

Reload reads the file again with every layer it sublayers, references or loads as
a payload, and re-keys the node. A file that is missing or does not parse sets a
node error carrying USD's diagnostic, and the scene action fails.

Tolerance: none recorded.
