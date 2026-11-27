# USDToGeo

Tolerance: none recorded.

USDToGeo turns prims of its scene input into geometry. Each prim becomes either one packed USD primitive, which holds the prim's path, transforms and bounds, or the prim's own polygons, points or curves.

## Choosing what is read

**Prim Pattern** matches the prims to read. **Traversal** then says which prims at or below each matched prim become geometry:

- **No Traversal**: each matched prim itself.
- **Components**: each prim of kind `component`.
- **Gprims**: each geometric prim (mesh, points, curves, shapes) and each point instancer. Use it to unpack a whole hierarchy.
- **Groups**: each prim of kind `group` or `assembly`.

Only the outermost prim of the chosen sort is taken below a matched prim, never one inside it, and a prim matched twice is read once.

**Purpose** lists the purposes read, separated by spaces: any of `default`, `render`, `proxy` and `guide`. The default reads everything but guides. A prim whose purpose is not listed is not read, and neither is anything below it. An unknown purpose is an error.

**Exclude Inactive Primitives** leaves inactive prims out. Off, they are read too, and the `usdactive` primitive attribute is 1 on the geometry of active prims and 0 on that of inactive ones. The attribute only exists when an inactive prim was read.

## Naming

- **Path Attribute**: the primitive string attribute holding the path of the prim each primitive came from; `path` by default. On a points prim it is a point attribute. Blank writes none.
- **Name Attribute**: the primitive string attribute holding the prim's name, the last element of its path. Blank, the default, writes none.

## Time

- **Time Sample**: **Animated** reads the scene at the frame being rendered. **Static** reads it at **Import Frame** whatever the frame, so the geometry is the same at every frame and is cached once.
- **Import Frame**: the frame a static sample reads.

## Packed primitives

With **Unpack** off, each prim becomes one packed USD primitive on one point.

- **Pivot Location**: where that point goes, **Centroid** (the centre of the prim's bounds in world space, the default) or **Origin** (the prim's origin in world space). The point does not move the contents.
- **Import Point Instances**: off, a point instancer is one packed primitive. On, each of its instances is a packed primitive on its prototype, placed by the instance's transform. Path Attribute and Name Attribute then name the prototype. The instancer's `ids`, `velocities`, `angularVelocities` and `accelerations` become the point attributes `id`, `v`, `w` and `accel`, and its per-instance primvars follow **Import Primvars**. Instances listed in `invisibleIds` get `usdvisibility` set to `invisible` (`inherited` on the others). Instances the instancer deactivates are left out unless inactive prims are read, when `usdactive` is 0 on them.

## Unpacked primitives

With **Unpack** on, meshes, points and curves are read as polygons, points and curves sharing one point pool. Other prims are skipped and listed in the node's message.

- **Flatten Transforms**: moves the points into world space by each prim's world transform and marks the geometry as world space. Off, the points stay in each prim's own space.
- **Apply Inverse USD Transform**: applies the inverse of each prim's world transform to points read in world space, so the points are in the prim's own space. It overrides Flatten Transforms, and the result is what Flatten Transforms off reads.
- **Import Primvars**: the primvars read, as space-separated globs over their USD names; a glob starting with `^` removes what it matches. `* ^displayColor` reads everything but the colour. `displayColor` becomes `Cd`.
- **Import Inherited Primvars**: also reads the constant primvars a prim inherits from its ancestors. The prim's own primvar wins over an inherited one of the same name.
- **Import Attributes**: custom attributes to read, as globs over their names. A custom attribute has no interpolation, so it becomes a point, vertex or primitive attribute when its array holds one value per point, vertex or primitive, and a detail attribute otherwise.
- **Non Transforming Primvars**: primvars read as plain values rather than points, vectors or normals, so transforming the geometry leaves them as they are; `rest` by default.
- **Translate ST To UV**: reads the `st` primvar as `uv`. Off, it stays `st`.
- **Write USD Xform**: writes each prim's local-to-world matrix as the 16-value `usdxform` primitive attribute (a point attribute on a points prim). It is not transformed with the geometry.
- **Import Material Path**: writes the path of the material bound to each prim as the `usdmaterialpath` primitive attribute. Faces of a GeomSubset with its own binding get the subset's material.

`usdxform` and `usdmaterialpath` record where the geometry came from. GeoToUSD never writes them back as primvars.

## Round trip

To edit a prim's geometry and put it back on the scene, read it here and write it with GeoToUSD, using the same scene as GeoToUSD's Scene input:

USDRead → USDToGeo (Unpack on, Traversal Gprims) → geometry edits → GeoToUSD (Scene input: the USDRead)

Leave Flatten Transforms off, or turn Apply Inverse USD Transform on, so the points stay in each prim's own space. The prims keep their paths, transforms and material bindings, and GeoToUSD writes only the values that changed. An unedited round trip gives back the input scene.
