# USDAssignMaterial

Binds one material to the prims of its Scene input that Prim Pattern names, in a
layer of its own. The pattern is a list of absolute prim paths separated by
spaces, whose names may use `*` (any run of characters) and `?` (any one
character): `/Asset/geo/*` names every child of `/Asset/geo`. A match's
descendants take the material too. The node does not touch colour.

The material comes from one of two places:

- Material input: connect a USDMaterial (or any scene holding a material). Its
  root prims are grafted under the project's Materials Scope (Project Settings,
  default `/materials`), so a USDMaterial at `/Material1` lands at
  `/materials/Material1`, and the first material in them, depth first, is bound.
  A root already named like the scope, such as a USDScopePrim at `/materials`,
  is passed through rather than nested. Several assigns sharing one material
  node compose into a single material. Material Path is greyed out while the
  input is connected. With the Materials Scope off (empty) the input has nowhere
  to go: a node error, and the scene action fails.
- Material Path: with the input unconnected, the path of a material already in
  the scene, such as `/materials/Metal`. A path that names no material in the
  input scene still authors the binding, with a node warning, so a material
  added further down the graph can satisfy it. With no material from either,
  the scene passes through with a warning.

Matches are grouped by their root prim. Each root gets one collection naming its
matches (`collection:<node>`, expanding to their descendants) and one
collection-based binding to the material
(`material:binding:collection[:<purpose>]:<node>`), both named after the node's
fully qualified name. The prims keep their binding when a later USDXformPrim,
USDScopePrim or other re-parenting node moves them, as the graft re-points one
relationship per root. Each binding is written as the strongest on its root, so
where two assigns name the same prims the later node in the graph wins.

Knobs:

- Prim Pattern: the prims to bind.
- Material Path: the material to bind when the Material input is unconnected.
- Purpose: which renders the binding is for. All (the default) is the binding
  every purpose falls back to; Full is for final-quality renders only; Preview is
  for interactive previews only, such as the Viewer.
- Strength: Stronger Than Descendants (the default) replaces every binding below
  the matched prims, including a material an asset or a USDCard binds itself to;
  Weaker Than Descendants leaves the prims below that have a binding of their
  own and only fills in where there is none.

Matches inside the Materials Scope are skipped and named in a node warning. A
pattern that matches nothing passes the input scene through unchanged and sets a
warning on the node. An empty pattern passes the scene through without a
warning. A pattern entry that is not an absolute path, or a Material Path that
is not an absolute prim path, sets a node error and the scene action fails.
Relationship targets or connections of the material that lie under no root of
the Material input are dropped and named in a node warning, as in USDXformPrim.

Tolerance: none recorded.
