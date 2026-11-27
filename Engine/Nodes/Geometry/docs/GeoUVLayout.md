# GeoUVLayout

GeoUVLayout packs the uv islands of its input into rectangles, UDIM tiles or the islands of the geometry on its **Targets** input. Each island is moved, turned and scaled as a whole; its shape in texture space never changes. The result is written back on **UV Attribute**, of the class it already has (vertex or point). Every other attribute and group passes through, and polygons outside **Group** keep their uv.

## Islands

- **UV Attribute**: the point or vertex attribute packed, `uv` by default. Its third component is kept, plus any offset.
- **Group**: the polygons packed, as a primitive group pattern. Blank packs every polygon.
- **Separator Edges**: an edge pattern. When set, the islands are the regions these edges cut the surface into, instead of the regions connected in uv.
- **Island Attribute**: a primitive attribute. Islands sharing a value of it are packed as one unit, keeping their places relative to each other. Merged units never stack.
- **Connectivity Tolerance**: how close in uv two vertices on one point must be for their polygons to share an island.

## Per-island overrides

Each override field takes space-separated `polygon:value` pairs. The polygon is a primitive number and names the island it belongs to. Later pairs win over earlier ones, and pairs win over the attribute. Pairs that do not parse, or name a polygon outside the group, are ignored with a warning.

- **Target Attribute** / **Target Overrides**: the target of an island: a rectangle number, a UDIM tile number, or a target island number. A negative value lets the packer choose. For example, `35:1009` sends the island of polygon 35 to tile 1009.
- **Island Scale Attribute** / **Scale Overrides**: a scale relative to the other islands.
- **Island Offset Attribute** / **Offset Overrides**: added to the third uv component, to keep stacked or layered islands apart.

## Before packing

- **Scale Islands To Match Surface Area**: scales each island so its uv area is proportional to the area of its polygons, giving every island the same texel density.
- **Axis Alignment**:
  - **None**: islands keep their orientation.
  - **By Island Symmetry** (default): turns the smallest rectangle round each island level, with its long side along u.
  - **By Island Position In 3D**: turns each island so the coordinate axes lying in its polygons' planes run along u and v. A curved island takes the turn its polygons agree on best, weighted by area.

## Packing

- **Scale**: **Largest Packable** makes the islands as large as they can be while all still fit, searched to **Scale Search Tolerance**. **Fixed** uses **Fixed Scale**. Islands that do not fit are lined up beside the targets and put in the nonpacked group. With UDIM tiles, a fixed scale fills as many tiles as it needs.
- **Rotation Step**: the turns tried for each island while packing: No Rotation, 180, 90 (default), 45, 22.5, 11.25, or **Custom** with **Custom Rotation Divisions** equal turns of the full circle.
- **Avoid Packing Over Non Group Islands**: packs round the islands of the polygons outside the group, which stay where they are.
- **Pack In Cavities**: lets islands go into the holes and notches of other islands.
- **Island Padding**: the space between islands, in pixels of the search resolution. **Pad Boundaries** keeps it from the target edges too.
- **Spread Islands**: spreads the islands over the whole target instead of packing them into a corner.
- **Iterations**: the number of packings tried with shuffled island orders, run in parallel; the tightest wins. **Random Seed** fixes the shuffles, so one seed always gives one layout.
- **Search Resolution**: the pixels along the long side of the largest target, or of a tile, at which the islands are rasterized. Higher packs tighter and slower.

## Targets

- **Pack Into**:
  - **Rectangles**: the rows of **Target Rectangles**, each a **Center** and a **Size** in uv, numbered from 0. One unit square by default.
  - **UDIM Tiles**: a grid of **Tile Size** tiles, **Columns** to a row, filled from **Starting UDIM**. Tiles past 1100 warn.
  - **Islands From Second Input**: the uv islands of the **Targets** input, numbered in the order found. They are found with **Target UV Attribute**, **Target Group**, **Target Separator Edges** and **Target Island Attribute**, as for the islands above. An island packed into one has its third uv component raised by the lowest third component of that target.
- **Use Default Target** / **Default Target**: the target of the islands that have none. Off, the packer chooses.

## Stacking

- **Stack Identical Islands**: places islands of the same shape on top of each other instead of side by side.
- **Match Mirrored Islands**: also stacks mirror images.
- **Allow Stacking On Non Group Islands**: lets islands stack on identical islands outside the group.
- **Shape Tolerance**: how much two shapes may differ, as a fraction of their size, and still stack.

## Outputs

- **Output Nonpacked** / **Nonpacked Group**: a primitive group, `nonpacked` by default, of the polygons of the islands that did not fit. On by default.
- **Output Island Number** / **Island Number Attribute**: an integer primitive attribute, `island`, holding each polygon's island number; -1 outside the group.
- **Output Assigned Target** / **Assigned Target Attribute**: an integer primitive attribute, `target`, holding the target each polygon's island went to; -1 when it was not packed or is outside the group.
