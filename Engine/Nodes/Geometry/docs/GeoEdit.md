# GeoEdit

Tolerance: none recorded.

GeoEdit moves points, edges and primitives of the geometry on its Source input by hand, in the 3D view. Pages no edit touches stay shared with the input.

## Editing in the 3D view

1. View the GeoEdit node itself: edits are made on the element numbers of the geometry it shows.
2. Pick points, edges or primitives with the viewer's pick mode.
3. Drag the X, Y or Z arrow of the handles drawn at the centre of what is picked. The geometry follows the drag, and releasing the mouse makes one undoable edit.

- Points move by the drag.
- Edges move both of their points.
- Polygons and curves move all of their points.
- Packed primitives move whole: their transform changes, and their pivot point stays where it is.

A second drag of the same elements adds to the first. **Clear Edits** removes every edit.

## When the input changes

Edits are stored by point and primitive number, together with the input's point and primitive counts when they were made. If the input later has a different number of points or primitives, the numbers may name other elements, so GeoEdit applies none of its edits, passes the input through and shows a warning with both counts. Reconnect the input the edits were made on, or clear the edits and start again.

## Normals

When the input has normals (N) on its points, vertices or primitives and an edit moves points, N is computed again on the same elements, weighted by polygon area with a 60 degree cusp angle for vertex normals.
