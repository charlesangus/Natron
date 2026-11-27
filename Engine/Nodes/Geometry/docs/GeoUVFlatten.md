# GeoUVFlatten

Tolerance: a flat quad flattens with a mean corner-angle error below 1e-6 radians; a bent strip of planar quads unfolds below 1e-4.

GeoUVFlatten cuts the polygons of its input into islands along seams, flattens each island into texture space with as little angular distortion as it can, and packs the islands into the packing area. The result is written on the vertex attribute **UV Attribute**. Non-polygon primitives and polygons outside **Group** keep their uv, and every other attribute and group passes through.

The node is procedural: every change to its controls is applied straight away, and there is no separate repack step.

## What is flattened

- **Group**: the polygons to flatten, as a primitive group pattern. Blank flattens every polygon.
- **UV Attribute**: the vertex attribute written, `uv` by default. An existing vertex attribute of the name keeps its storage and its other components (such as w). A point attribute of the name is replaced by a vertex one seeded from it.
- **Flattening Method**:
  - **Spectral (SCP)**: fast and robust, with slightly more area distortion.
  - **Angle Based (ABF)**: slower and more even in area, and the only method that takes straightened loops. An island it cannot solve is flattened with Spectral instead, with a warning.

Polygons with no area are skipped, with a warning giving their number.

## Existing UVs

These apply when the input already has the uv attribute.

- **Preserve Seams**: also cuts the islands wherever the existing uv breaks across an edge.
- **Preserve Layout**: leaves the islands that already have uv (some area in texture space) where they are. The new islands are packed round them.
- **Preserve Island Boundary Shapes**: Spectral only. Keeps the boundary of each island that already has uv and flattens its inside again. With Preserve Layout the island stays in place; without it, it is packed like the others.

## Flattening constraints

- **Seams**: the edges the islands are cut along, as an edge pattern: point paths such as `p3-4-5`, edge group names and `@attribute` comparisons. Unlike a group, a blank field cuts no edge.
- **Rectify Group**: a region of quads to make rectangles in uv, as a vertex group of vertex edges. A vertex names the edge from it to the next vertex of its polygon, so each side of a seam has its own. Listing the horizontal edges is enough; the rest of the region follows.
- **Enable Pins** and **Pins**: each row pins one vertex. **Primitive** is the polygon's number and **Vertex** the vertex's place in it, from 0. **UV** is where it goes, written `u v` or `u, v`. **Reference UV** is where the polygon's next vertex goes; it is only used when the pin is the only one in its island, to set the island's size and turn. An island with pins is left where its pins put it, and the packing goes round it.
- **Enable Axis Alignment** and **Alignments**: each row lines up a vertex group. **Group** is a vertex group pattern; naming one of the vertices wired to a point is enough. **Align Parallel To** is `U`, giving the vertices one v, or `V`, giving them one u.
- **Enable Loop Straightening** and **Straightenings**: Angle Based only. Each row's **Group** is a vertex group of vertex edges put on one straight line.

## Layout constraints

- **Enable Manual Layout**: on, the default, the Seams and the flattening constraints are applied before packing and **Layout Seams** is ignored. Off, the islands that **Layout Seams** cuts are flattened without constraints and packed first. The islands that **Seams** cuts are then flattened with their constraints, and each is fitted by turning, uniform scaling and moving onto where the packing put its vertices. Edges in Seams but not in Layout Seams are cut, and edges in Layout Seams but not in Seams are joined, without the rest of the layout moving.
- **Layout Seams**: the edges cut before packing when Enable Manual Layout is off. Blank cuts no edge.

## Output groups

- **Output Seams** and **Output Seams Group**: writes the edges the islands are cut along as an edge group, `seams` by default.
- **Output Constrained Islands** and **Output Constrained Islands Group**: writes the polygons of every island with a pin, alignment or straightened loop as a primitive group, `constrainedprims` by default.

## Packing area

- **Center** and **Size**: the rectangle in uv the islands are packed into, centred on (0.5, 0.5) with size 1 by default.

The packing keeps each island's orientation, so alignments and rectified quads stay on the axes. Islands without them are first turned to their smallest bounding rectangle. Islands are scaled together as large as they all still fit; one that does not fit is set beside the area, with a warning. For control over rotation, scale, targets and stacking, follow with GeoUVLayout.
