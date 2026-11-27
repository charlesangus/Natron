# GeoToUSD

Tolerance: none recorded.

GeoToUSD puts the geometry of its Geometry input on the scene, over the optional Scene input. A path attribute on each primitive names the prim it becomes. Primitives without one go under **Path Prefix**. **Other Primitives** decides whether a prim is defined anew or laid over the one the Scene input already has.

## Where the prims go

- **Path Attributes**: the primitive attributes that name prims, separated by commas or spaces (default `path,name`). For each primitive, the first listed attribute with a non-empty value wins. Characters a prim name cannot hold become `_`, so `left leg` becomes `left_leg`.
- **Path Prefix**: the prim that relative paths, and geometry with no path, go under. It defaults to `/` followed by the node's name, and follows the node when it is renamed. A value set by hand stays.
- **Prefix Absolute Paths**: off, a path starting with `/` is used as it is, so `/a/b` stays `/a/b`. On, it goes under the prefix too, so `/a/b` becomes `/pre/a/b`.

Geometry with no path is named by its type under the prefix: `mesh_0`, `curves_0`, `points_0`, `instancer_0` and so on.

Primitives of different types (a mesh and curves, say) sharing one path cannot be one prim. Each becomes a sibling with a number added, `/a/b_0` and `/a/b_1`, and the node warns. Primitives of one type sharing a path are merged into one prim.

## Defining and overlaying

**Other Primitives** applies to meshes, curves and points:

- **Define** (the default): each prim is a `def`. On a path the Scene input already has, it redefines that prim: its type and the properties GeoToUSD writes replace the input's, and properties it does not write still come from the input.
- **Overlay**: each prim is an `over` carrying the geometry, which changes the input's prim in place. A prim the Scene input lacks has nothing to change, so it is not drawn. The node warns and lists those paths.
- **Overlay Transforms**: each prim is an `over` carrying only its transform (`xformOp:transform`), so the input's geometry and properties are kept. Meshes, curves and points carry no transform of their own, so the over writes the input prim's own local transform, or the world origin for a prim the input lacks.

With Define, geometry read from USD whose prim is no longer in the Scene input (a pruned prim, say) goes under the prefix rather than defining a prim at its old path. The node lists those paths.

- **Define Only Leaf Primitives**: on, the prims above each geometry prim are written as `over`, so they only exist where the Scene input already has them.
- **Parent Primitive Type**: the type of the prims written above the geometry: **Xform**, **Scope** or **None** (typeless). It applies only where the Scene input lacks the prim. A prim the input holds is left with its own type.
- **Kind Authoring**: the model kinds written on the prims created above the geometry (only those the Scene input lacks). **One Component** marks the topmost of them, usually the prefix, as a component. **Nested Groups And Components** marks the prims directly holding geometry as components and the prims above them as groups. **Nested Assembly, Groups, And Components** does the same, but marks the topmost as an assembly. **None** writes no kind.

## Packed geometry

- **Packed USD Primitives**: what packed USD primitives (as USDToGeo writes with Unpack off) do to the prim they were read from. **Ignore** writes nothing. **Overlay Transforms** writes the moved transform (the default). **Overlay Transforms And Attributes** also writes their primitive attributes as constant primvars.
- **Packed Primitives**: what packed geometry becomes. **Create Xforms** gives each packed primitive an Xform holding its geometry. **Native Instances** gives each one an instanceable prim sharing one prototype. **Point Instancer** (the default) gives one point instancer per path. **Unpack** writes the geometry as plain meshes, curves and points. Sheared packed geometry is written as native instances, because a point instancer cannot hold shear.

## Transforms

- **Adjust Transforms For Input Hierarchy** (on): geometry keeps its world position under the Scene input's prims. Each prim's points or transform are written relative to the world transform of its ancestors in the input, so a mesh at the origin under a parent moved by (1, 0, 0) stays at the origin. Off, the geometry is written as it is and moves with its ancestors. Geometry not read from USD, and geometry USDToGeo flattened into world space, count as world space. Geometry read from USD in its own space keeps it.

## Loading as a reference

- **Load As Reference**: instead of adding the geometry layer to the scene, it is brought in through an arc on the Path Prefix prim. Prims on the way to the prefix that the Scene input lacks are defined. The prefix keeps its own path, and the geometry layer is converted without the Scene input. Geometry is placed as written, Other Primitives applies within the layer, and Adjust Transforms is not used.
- **Reference Type**: **Weaker Payload** (the default), **Stronger Payload**, **Weaker Reference** or **Stronger Reference**. A payload can be left unloaded. Weaker arcs are appended to the prim's list and stronger ones are prepended, which matters when the prim already has arcs.
- **Reference Primitive**: the prim of the geometry layer the arc targets. **Automatic** takes the **Path Prefix** prim, so the geometry lands directly under the prim that holds the arc, else the layer's first root Xform, else its first root prim. **Default** targets the layer's default prim, and warns when the layer has none. **Specific** targets **Reference Primitive Path**.
- **Make Instanceable**: marks the prefix prim instanceable.
- **Parent Primitive Kind**: the kind of the prims created above the prefix (they also take Parent Primitive Type).

## Layer

- **Layer Style**: **Sublayer** (the default) adds the geometry to the scene as a layer of its own, which is converted once and shared. **Copy Into Node Layer** converts the geometry and copies it into the node's own layer, which a USD writer saves along with the rest of the node's opinions.
- **Author Time Samples**: with Copy Into Node Layer, writes the geometry at every frame of the project range.
- **Default Point Width**: the width of points with no `pscale` attribute.

## Choosing what is converted

**Group** restricts the conversion to part of the geometry. It takes the same patterns as the other geometry nodes: element numbers and ranges (`0-10`, `0-20:2`), group names and globs, `*`, attribute comparisons (`@P.x<0`), and the `!` and `^` prefixes. A blank group converts everything.

**Group Type** says whether the pattern selects points or primitives. **Guess from Group** takes the class of the group the pattern names, and primitives for plain numbers. Edge patterns are an error.

- **Primitives**: the selected primitives, and the points they use. Loose points, used by no primitive, are left out.
- **Points**: the primitives whose points are all selected, with their points, and the selected loose points. A primitive with only some of its points selected is left out, and so are those points.

Packed primitives, packed USD primitives and loose points are selected like any other element. Nothing outside the selection is written.

A kept primitive becomes the prim it would without a group, so changing the group does not move prims that stay. Some names follow from what is converted rather than from the primitive itself:

- Primitives of several types sharing one path become numbered siblings (`_0`, `_1`). When the group keeps only one type, it takes the path itself.
- With **Native Instances**, the instances of a path are numbered `instance_0`, `instance_1` and so on, in the order of the kept packed primitives.
- The prototypes of a point instancer are numbered in the order their embedded geometry first appears among the kept packed primitives.

Select one of these by a path pattern over the parent prim when the group may change.

## Node message

After each evaluation the node message lists where the geometry went: up to 20 prims, each with its path and whether it was **defined** or **overlaid**. It also lists overlays the Scene input has no prim for, geometry whose source prim is gone, and sheared packed geometry. The message is a warning when any of those lists is non-empty, and information otherwise.

## Editing a scene's geometry in place

To change geometry and put it back where it came from:

1. **USDRead** the scene.
2. **USDToGeo** with the prims to edit, wired to the USDRead. Turn on **Unpack** to edit the points.
3. Edit the geometry, with **GeoTransform** for example.
4. **GeoToUSD** with the edited geometry on its Geometry input and the USDRead on its Scene input. Set **Other Primitives** to **Overlay**. The `path` attribute USDToGeo wrote puts each prim back at its original path. Adjust Transforms keeps the parent prims' transforms intact.
5. **USDMerge** the GeoToUSD output over the scene, or use the GeoToUSD output directly. It already holds the Scene input.

With Define instead of Overlay, the edited prims redefine the originals. That also works, but the type and every property GeoToUSD writes replace the input's.
