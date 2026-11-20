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

Prune Unselected (off by default) turns the node around to isolate the
selection: it keeps what Prim Pattern selects and prunes the rest of the scene,
by Deactivate or Hide as Mode says.

- The matches survive with their ancestors and descendants.
- Keep Lights, Keep Cameras, Keep Materials and Keep Render Settings (all on by
  default, and available only while Prune Unselected is on) also keep, with
  their ancestors: every light and light filter; every camera; every material
  with the shaders and node graphs inside it, so kept prims still resolve their
  bindings; and every render settings, render product, render var and render
  pass prim.
- The node authors on the fewest prims it can: the highest prims that lie off
  every kept path. A kept prim's ancestors get no opinion at all, so in Hide
  mode they stay visible, unless the input already made one invisible.
- In Hide mode, a pruned prim that cannot be drawn, such as a material or a
  typeless prim, is not hidden itself; the highest prims below it that can be
  drawn are hidden instead.
- USD cannot author inside an instance, so an instance holding a match is kept
  whole, and an instance off every kept path is pruned as one prim.

In a viewer that leaves payloads unloaded, the node cannot see inside an
unloaded branch: it keeps one when the pattern could match inside it, judged
from the pattern's text, and prunes it otherwise. The Keep checkboxes only look
at loaded prims, so a light inside an unloaded branch the pattern cannot reach
is pruned in the viewer. Loading or unloading anything re-evaluates the node.
Renders load everything, so what they prune is exact.

A pattern that matches nothing passes the input scene through unchanged and sets
a warning on the node, with Prune Unselected on as well; in a viewer, a pattern
that finds nothing loaded but reaches unloaded branches still isolates them. An empty pattern passes the scene through without a
warning. A pattern entry that is not an absolute path sets a node error and the
scene action fails.

Tolerance: none recorded.
