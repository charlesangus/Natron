# GeoForEachBegin and GeoForEachEnd

Tolerance: none recorded.

A geometry loop runs the nodes between one or more GeoForEachBegin nodes and a GeoForEachEnd
below them once per iteration, and gathers the results. An iteration can be one piece of the
geometry, one point, or one step of a count.

## Pairing

The loop belongs to the GeoForEachEnd. Walking up from its Source input, every GeoForEachBegin
reached opens the loop. A GeoForEachEnd met on the way closes a loop nested inside this one, so
the GeoForEachBegins above it belong to that loop. A GeoForEachBegin that no GeoForEachEnd
closes, or that several do, outputs its input and shows a warning.

## GeoForEachBegin

- **Source** is the geometry this node works on.
- **Method** chooses what it outputs in each iteration:
  - **Extract Piece or Point**: the elements of Source the iteration is about. Primitive pieces
    keep the points their primitives use. Point pieces keep the primitives whose points are all in
    the piece. Attributes, groups, edge groups between kept points, and packed geometry come
    along. Pieces are matched by the value the GeoForEachEnd splits them by: the attribute value,
    or the piece's number under Connectivity and Each Element. A piece that Source lacks gives
    empty geometry. Under Iteration Method Count it outputs Source.
  - **Fetch Feedback**: under Gather Method Feedback, the result of the iteration before, and
    Source in the first iteration.
  - **Fetch Metadata**: a geometry with no points or primitives and five detail attributes:
    `iteration` (int), `numiterations` (int), `value` (float), `ivalue` (int) and `svalue`
    (string). Source is not used.
  - **Fetch Input**: Source, the same in every iteration.

Several GeoForEachBegins may open one loop, each with its own Method. For example, one extracts
the piece and another fetches the metadata, which a GeoWrangle reads on its second input.

Outside a cook of the loop, for example when a GeoForEachBegin is viewed on its own:

- Extract Piece or Point shows the piece Single Pass Iteration names when Single Pass is on, or
  the first piece.
- Fetch Metadata shows the first iteration.
- Fetch Feedback shows Source.

## GeoForEachEnd

- **Source** is the end of the loop body.
- **Piece** is optional. When it is connected, its pieces are the ones iterated over. Otherwise
  the pieces come from the Source of the first GeoForEachBegin set to Extract Piece or Point.

Parameters:

- **Iteration Method**: Pieces or Points runs once per piece. Count runs Iterations times.
- **Gather Method**: Merge merges every iteration's result in iteration order, as GeoMerge does,
  and cooks the iterations in parallel. Feedback starts each iteration from the result of the one
  before and outputs the last result. Feedback iterations cook one after the other.
- **Piece Elements**: whether a piece is made of primitives or of points.
- **Piece Method**:
  - Attribute groups elements by the value of Piece Attribute.
  - Connectivity groups elements joined through shared points.
  - Each Element makes every element its own piece.

  Pieces are numbered in the order their first element appears.
- **Piece Attribute**: an integer or string attribute of the Piece Elements class, `name` by
  default. If the attribute is missing, the node shows an error naming it.
- **Iterations**, **Start Value** and **Increment** set up Count: iteration *k* has value
  Start Value + *k* × Increment.
- **Max Iterations** is a safety cap. More iterations than this is an error.
- **Single Pass** cooks only the iteration **Single Pass Iteration** names, counted from 0. Under
  Feedback that iteration starts from the GeoForEachBegin's Source. Naming an iteration past the
  last is an error.
- **Stop Condition** is a Python expression, written as parameter expressions are. It is
  evaluated before each iteration, with that iteration's `loop.*` variables and the
  `loops.<name>.*` variables of this loop and the loops around it. The loop ends before the first
  iteration the expression is true for. It may read parameters but not geometry. If it raises an
  exception, the node shows an error. Leave it empty to run every iteration.
- **Stop Attribute**, under Feedback, names an integer detail attribute. When an iteration's
  result has a non-zero value there, the loop ends and that result is the output.
- **Loop Name** names this loop's `loops.<name>.*` variables. If it is empty, the node's script
  name is used.

With no pieces or no iterations, the output is empty geometry and the node shows an info message.

## Loop variables

Each iteration sets these context variables, as `loop.*` and as `loops.<name>.*`:

| Variable | Count | Pieces by integer | Pieces by string, Connectivity, Each Element |
|---|---|---|---|
| `iteration` | *k* | *k* | *k* |
| `numiterations`, `count` | Iterations | number of pieces | number of pieces |
| `t` | *k* / (count − 1) | same | same |
| `value` | Start Value + *k* × Increment | the attribute value | *k* |
| `ivalue` | `value` rounded | the attribute value | *k* |
| `svalue` | empty | empty | the string value, or empty |

Knob drivers read these variables, so a GeoTransform with its translate driven by
`loop.iteration` moves each iteration's geometry by a different amount. A GeoWrangle reading
`detail(1, "iteration")` from a Fetch Metadata GeoForEachBegin sees the same values.

Each distinct piece and each iteration cooks, and is cached, separately. The nodes in the body
that read no loop variable and sit downstream of no GeoForEachBegin that depends on the
iteration cook once and are shared by every iteration.
