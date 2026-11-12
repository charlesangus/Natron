# GeoPython

Tolerance: none recorded.

GeoPython runs a Python script on the geometry on its Source input. The script reads and writes attributes and groups through `geo`, can add and remove points and primitives, and can read the current frame as `frame`. It runs once per render of the node, over the whole geometry, in a single thread, so for a per-element operation on a large mesh prefer a numpy expression to a Python loop.

## Inputs

- **Source** is the geometry the script edits. With nothing connected, the script runs over empty geometry, so it can still create points and primitives.

## Parameters

- **Script** holds the Python. A blank script passes Source through unchanged.

The script runs in a fresh namespace each time, with the standard builtins and `import` available. Nothing a script defines survives to the next render. Two names are predefined:

- **`geo`** is the geometry being edited.
- **`frame`** is the current frame, as a float. A script that mentions `frame` anywhere is treated as changing from frame to frame, so its result is not reused between frames.

## Element classes

`geo` has four element classes. Each holds the attributes and groups of one kind of element.

- **`geo.points`** holds point attributes.
- **`geo.vertices`** holds vertex attributes.
- **`geo.prims`** holds primitive attributes.
- **`geo.detail`** holds attributes of the whole geometry. It has one element and no groups.

Elements are numbered from 0 in the order they appear. Each class supports:

- `len(c)` is the number of elements.
- `c["name"]` reads an attribute.
- `c["name"] = values` writes an attribute.
- `del c["name"]` removes an attribute.
- `"name" in c` tells whether the attribute exists.
- `c.attributes()` is the list of attribute names.
- `c.attribute_info("name")` is a dict with `storage` (`int32`, `int64`, `float32`, `float64` or `string`), `tuple_size`, `type_info` (`none`, `vector`, `normal`, `point`, `colour`, `matrix` or `quaternion`) and `is_array`.

Reading an attribute that doesn't exist raises `KeyError`. Attributes that hold an array per element cannot be read or written by a script.

## Reading and writing attributes

A numeric attribute reads as an array with one entry per element, in element-number order. Indexing it gives a number, or a tuple for an attribute with more than one component. Example: `geo.points["P"][0]` is the first point's `(x, y, z)`.

Writing replaces the values, and the whole attribute is written back when the script ends. Accepted values are:

- a single number, which fills every component of every element;
- a sequence with one value per element, each a number or a tuple;
- a flat sequence of numbers as long as the total component count, which fills the elements in order (this is how to write a single element's tuple on `geo.detail`);
- an array from `geo`, or any numpy array.

A value of the wrong length raises `ValueError`. An integer attribute refuses floats with `TypeError`. A number too large for an `int32` attribute raises `OverflowError`.

Assigning to a name the class doesn't have creates the attribute, with its type taken from the value: an `int` or an integer array makes an int32 attribute, a `float` or a float array a float32 one (float64 for a float64 numpy array), and a `str` or a sequence of strings a string attribute. The tuple size comes from the shape of the value.

A string attribute reads as a plain list of strings, or of tuples of strings. Assign a list of the same length, or one string to fill every element.

An attribute array also has:

- `a.tolist()` is the values as a list of numbers, or of tuples;
- `a.tuple_size`, `a.dtype` (the numpy name), `a.shape` and `a.readonly`.

Elements can be assigned one at a time with `a[i] = value`, but not deleted. Use `geo.remove_points` or `geo.remove_prims` to remove elements.

Example, to lift every point by its distance from the origin:

```
import numpy
p = numpy.asarray(geo.points["P"])
p[:, 1] += numpy.linalg.norm(p, axis=1) * 0.1
```

## numpy

Arrays support the buffer protocol. With numpy installed, `numpy.asarray(geo.points["P"])` is a writable view of the positions, and writing through it changes the attribute. An array that is read only, such as those returned by the topology calls, gives a read-only view. numpy is not required for anything else.

## Groups

Groups are available on `geo.points`, `geo.vertices` and `geo.prims`. A group reads as a bool array with one entry per element.

- `c.groups()` is the list of group names.
- `c.group("name")` is the membership, one bool per element. An unknown group raises `KeyError`.
- `c.set_group("name", mask)` creates or replaces a group. `mask` is one truth value per element, or a single value for all of them.
- `c.remove_group("name")` removes a group. An unknown group raises `KeyError`.

Example:

```
p = geo.points["P"]
geo.points.set_group("high", [pt[1] > 1.0 for pt in p])
```

## Topology

These read the connections between points, vertices and primitives. The results are read only.

- `geo.prim_types()` is each primitive's type name: `polygon`, `polyline`, `packed_geometry`, `packed_usd` or `unknown`.
- `geo.prim_vertex_counts()` is each primitive's vertex count.
- `geo.prim_vertices()` is every primitive's vertex numbers, one primitive after another.
- `geo.prim_points(i)` is the point numbers of primitive `i`'s vertices, as a tuple. A negative `i` counts from the end.
- `geo.vertex_points()` is each vertex's point number.
- `geo.vertex_prims()` is each vertex's primitive number.

## Adding and removing elements

Element counts do not change while the script runs, so `len(geo.points)` is the same at the end as at the start. The calls below are queued and applied when the script ends, after the attribute writes. New elements are numbered after every existing one.

- `geo.add_points(n)` adds `n` points at the origin. `geo.add_points(positions)` adds one point per position. Both return the numbers the new points will have.
- `geo.add_prim(points, type="polygon")` adds one primitive through the point numbers in `points`, which may be new points, and returns its number. `type` is `"polygon"` or `"polyline"`.
- `geo.add_prims(counts, points, type="polygon")` adds one primitive per entry of `counts`, each taking that many point numbers from `points` in turn. The counts must add up to the length of `points`. It returns the new numbers.
- `geo.remove_points(numbers)` removes points and every primitive that uses them.
- `geo.remove_prims(numbers, delete_unused_points=False)` removes primitives, and with `delete_unused_points` the points nothing else uses.

A point number passed to `add_prim` or `add_prims` can be an existing point or one the script has already added. A number passed to a remove call must be one the geometry had when the script started. A number that doesn't exist raises `IndexError`.

Example, to add a triangle:

```
a, b, c = geo.add_points([(0, 0, 0), (1, 0, 0), (0, 1, 0)])
geo.add_prim([a, b, c])
```

## Errors

An exception, a syntax error, or a value that no longer fits its attribute when the script ends stops the render. The node shows the Python traceback, with the offending script lines, as its error. For example, a `ValueError` raised on line 2 shows `line 2` in the traceback.

Aborting a render stops the script between Python statements.
