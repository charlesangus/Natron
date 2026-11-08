# GeoWrangle

Tolerance: none recorded.

GeoWrangle runs a short program, the snippet, over the geometry on its Source input. The snippet is written in the geometry kernel language. It runs once for each element of the Run Over class, and many elements run at once on all cores, so a snippet only ever sees the element it is running for.

## Inputs

- **Source** is the geometry the snippet reads and changes. With nothing connected, the snippet runs over empty geometry, so a detail snippet can still create points and primitives.
- **Second** is optional geometry the snippet can read but not change, through `@opinput1_name` and the functions that take an input number of 1.

## Parameters

- **Run Over** chooses what the snippet runs once for: each point, each primitive, each vertex, or the geometry as a whole (Detail). `@name` reads and writes attributes of that class.
- **Group** limits the run to the members of a group of the Run Over class. Elements outside it keep their values. If the group doesn't exist, the node shows an error. Group is ignored when Run Over is Detail.
- **Snippet** holds the program. An empty snippet passes Source through unchanged.
- **Create Parameters** adds a user parameter for every name the snippet reads with a `ch` function that the node lacks. Parameters that already exist are left alone, so pressing it twice adds nothing the second time.

The snippet compiles when it changes, not on every frame. A compile error stops the render and shows on the node with its line and column, for example `line 2, column 9: unknown parameter 'amp'`. Errors found when the snippet runs, such as writing a point attribute from a primitive run, show the same way.

## The language

A snippet is a list of statements in a C-like syntax, run once per element.

- **Types:** `int`, `float`, `vector2`, `vector`, `vector4`, `matrix3`, `matrix` (4x4) and `string`, plus arrays of each, declared `float weights[]`.
- **Statements:** declarations, `=` and the compound assignments, `++` and `--`, `if`/`else`, `for`, `foreach`, `while`, `do`/`while`, `break`, `continue`, `return` and `{ }` blocks.
- **Functions:** a snippet can define its own at the top level, `float twice(float x) { return 2 * x; }`. Arguments are passed by reference unless declared `const`. Recursion is an error.
- **Expressions:** the C operators with C precedence, and `?:`.
  - Swizzles: `.x .y .z .w`, `.r .g .b .a`, and combinations such as `.xyz`.
  - Indexing a vector or an array: `v[i]`.
  - Brace literals: `{1, 2, 3}`.
  - Casts written as calls: `vector(x)`, `int(f)`.
- **Conversions:**
  - An int converts to a float, and a float converts to an int by truncating.
  - A scalar converts to any vector by filling every component.
  - A scalar converts to a matrix as a scaled identity.
  - Arithmetic between a scalar and a vector or matrix works componentwise.
  - `vector * matrix` transforms a row vector. A vector times a 4x4 matrix is transformed as a point.

## Attributes

`@name` is the attribute `name` of the element being run. Reading an attribute that doesn't exist gives zero, and writing one creates it on the Run Over class.

- **Type prefixes** set the type: `f@` float, `i@` int, `u@` vector2, `v@` vector, `p@` vector4, `s@` string, `3@` matrix3, `4@` matrix. Add `[]` for arrays, as in `f[]@weights`.
- **Well-known names** without a prefix take their usual type:
  - vectors: `P`, `N`, `v`, `Cd`, `uv`, `up`, `accel`, `rest`, `force`, `scale`, `pivot`, `trans`, `center`, `w`;
  - vector4: `orient` and `rot`;
  - int: `id`;
  - strings: `name` and `instance`.

  Any other unprefixed name is a float.
- **Where a read looks:** a point or primitive run reads that class first, then the detail. A vertex run also looks at the vertex's point and its primitive before the detail.
- **Writes** only go to the Run Over class.
- **Second input:** `@opinput1_name` reads the attribute from the Second input, at the same element number. These reads can't be written.

## Builtin variables

All of these are read-only.

- `@ptnum`, `@primnum` and `@vtxnum` are the current point, primitive and vertex numbers, or -1 when the run has none.
- `@elemnum` is the number of the element in the Run Over class.
- `@numpt`, `@numprim`, `@numvtx` and `@numelem` are the element counts.
- `@Frame` is the frame being rendered, and `@Time` is that frame in seconds at the project frame rate.

A snippet that reads `@Frame` or `@Time` renders anew on every frame.

## Geometry functions

Input numbers and attribute or group names must be literals. Input 0 is Source and input 1 is Second. Only input 0 can be changed.

- **Reading other elements:**
  - `point(input, "name", ptnum)`, `prim(input, "name", primnum)`, `vertex(input, "name", vtxnum)` and `detail(input, "name")` read one value of any non-array type.
  - `npoints(input)`, `nprimitives(input)` and `nvertices(input)` count elements.
- **Searching:**
  - `nearpoint(input, position[, maxdistance])` returns the closest point, or -1.
  - `nearpoints(input, position, maxdistance[, maxcount])` returns an array of the points within a radius, closest first.
- **Groups:**
  - `inpointgroup(input, "group", ptnum)` and `inprimgroup(input, "group", primnum)` test membership.
  - `setpointgroup(0, "group", ptnum, value[, "mode"])` and `setprimgroup(0, "group", primnum, value[, "mode"])` change it. The mode is `set`, `add`, `remove` or `toggle`.
- **Topology:**
  - `addpoint(0, position)` and `addpoint(0, ptnum)` add a point. The second form copies the attributes of an existing point.
  - `addprim(0, "poly" or "polyline"[, points...])` adds a primitive. The points can be up to eight point numbers or an int array.
  - `addvertex(0, primnum, ptnum)` adds a vertex to a primitive.
  - `removepoint(0, ptnum)`, `removeprim(0, primnum, andpoints)` and `setprimvertex(0, primnum, vertex, ptnum)` remove or rewire elements.

Topology edits are queued and applied after every element has run, so the elements being run never move under the snippet. Reading an element number that doesn't exist gives zero.

## Parameter functions

These read the node's user parameters at the frame being rendered, so animating a parameter animates the result. The name must be a literal, and it is checked when the snippet compiles. A name the node lacks, for instance after a parameter is renamed, is a compile error that names it.

- `ch("name")` and `chf("name")` read a number as a float.
- `chi("name")` reads a number as an int.
- `chv("name")` reads up to three components as a vector. Missing components are zero.
- `chs("name")` reads a string parameter.
- `chramp("name", x)` evaluates a curve parameter at `x`.

Integer, float, colour, checkbox and choice parameters are numbers.

## Language functions

- **Maths:**
  - trigonometry: `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`;
  - powers and logarithms: `sqrt`, `pow`, `exp`, `log`, `log10`;
  - rounding: `floor`, `ceil`, `round`, `trunc`, `frac`;
  - other: `abs`, `sign`, `min`, `max`, `clamp`, `degrees`, `radians`.
- **Remapping:** `lerp`, `fit`, `efit`, `fit01`, `fit10`, `fit11` and `smooth`.
- **Vectors:** `dot`, `cross`, `length`, `length2`, `distance`, `distance2`, `normalize` and `getcomp`.
- **Matrices and rotations:**
  - `ident`, `invert`, `transpose` and `determinant`;
  - `quaternion`, `qmultiply` and `qrotate`.
- **Random values:** `rand` and `noise`, which return a float or a vector depending on what the context expects.
- **Strings:**
  - `strlen`, `concat`, `sprintf`, `startswith`, `endswith`;
  - conversions: `atoi`, `atof`, `itoa`.
- **Arrays:**
  - `array`, `len`, `resize`, `push`, `append` and `set`.

## Example

Run over Points to raise each point by an animated amount that fades with height:

```
float fade = chramp("falloff", @P.y);
@P.y += chf("amp") * fade;
```

Press Create Parameters to add `amp` as a float parameter and `falloff` as a curve.
