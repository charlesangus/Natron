# GeoGroup

Tolerance: none recorded.

GeoGroup creates or edits a named group of points, primitives, vertices or edges on the geometry of its Source input. Only groups are written; every attribute stays shared with the input.

## Selecting

- **Group Name** and **Group Type** choose the group and the kind of element it holds.
- **Initial Merge** combines the selection with the members a group of that name already has: replace them, union, intersect or subtract.
- **Use Pattern** selects with a pattern of element numbers and ranges, group names or globs, attribute comparisons and the `!` and `^` prefixes. An empty pattern selects everything.
- **Use Bounding** keeps what lies inside a box or an ellipsoid. With both tests on, an element must pass both. With neither on, the selection is empty.

## Expand

**Expand** grows the group by **Expand Steps** rings of neighbours. A negative count shrinks it instead, removing the elements on the group's boundary. The open border of a mesh does not erode a group.

- Points neighbour each other through a shared edge.
- Primitives neighbour each other through a shared edge.
- Edges neighbour each other through a shared point.

## Promote

**Promote** converts the group into a group of the same name holding another kind of element, chosen by **Promote To**. It runs after Expand.

- Points to edges or primitives: the edges or primitives built on the points. With **Entirely Contained** on, all of an element's points must be in the group; otherwise one is enough.
- Edges to points: the endpoints of the edges. Edges to primitives: the primitives that use any of the edges.
- Primitives to points or edges: every point or every edge of the primitives.

Entirely Contained only matters when promoting from points. The group of the original kind is removed unless **Keep Original** is on. Promoting to the kind the group already holds changes nothing.

Expand and Promote need a Group Type of points, edges or primitives; with vertices the node shows an error.
