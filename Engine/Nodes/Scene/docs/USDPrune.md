# USDPrune

Removes the prims of its scene input that Prim Pattern names, by authoring an
opinion over them in its own layer; nothing the input authors is deleted. The
pattern is a list of absolute prim paths separated by spaces, whose names may use
`*` (any run of characters) and `?` (any one character): `/World/Geo/*` names
every child of `/World/Geo`. The node does not touch colour.

Mode sets how the matches are removed:

- Deactivate (the default) authors `active = false` on each match. The match and
  its descendants leave the composed stage: they are not drawn or rendered, and
  later nodes' patterns do not match them.
- Hide authors `visibility = invisible` on each match that can be drawn (an
  imageable prim). The match and its descendants stay on the stage, undrawn; a
  later node can match them and author them visible again.

A pattern that matches nothing passes the input scene through unchanged and sets
a warning on the node. An empty pattern passes the scene through without a
warning. A pattern entry that is not an absolute path sets a node error and the
scene action fails.

Tolerance: none recorded.
