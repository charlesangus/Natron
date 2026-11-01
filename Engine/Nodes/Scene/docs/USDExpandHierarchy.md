# USDExpandHierarchy

Sets how the viewer draws the prims of its scene input that Prim Pattern names,
for every viewer showing this node or a node below it. The pattern is a list of
absolute prim paths separated by spaces, whose names may use `*` (any run of
characters) and `?` (any one character). Renders, and the scene's prims and
attributes, are unchanged: the node authors only custom data
(`natron:viewerFold`, `natron:viewerOpenLevels`) over the matches.

Mode:

- Fold draws each match as its bounding box and unloads the payloads below it.
- Expand (the default) draws each match and everything below it, lifting folds
  a USDRead's Open Mode or an earlier node made below it.
- Expand Levels draws each match and Levels levels below it, and folds the
  prims deeper than that. Levels 0 draws the match and folds its children.

Expanding a match also opens each of its ancestors, whose other children stay
as they were. Folds and expansions made in the Scene Graph panel apply on top
until the viewer shows another node.

A pattern that matches nothing passes the input scene through unchanged and
sets a warning on the node. An empty pattern passes the scene through without a
warning. A pattern entry that is not an absolute path sets a node error and the
scene action fails.

Tolerance: none recorded.
