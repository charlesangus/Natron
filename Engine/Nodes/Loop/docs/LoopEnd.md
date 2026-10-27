# LoopEnd

Tolerance: none recorded. The fold runs on the CPU, whatever the GPU placement, until
M259 - GPU Placement Under Render Contexts.

- `LoopEnd`: renders its input, the loop body, once per iteration and folds the iterations
  together. The body starts at the LoopBegin found walking up from the input; none, several,
  or a LoopBegin also reached from another LoopEnd is an error, and the output is black.

## Variables

- Each iteration `k` (0 to `n - 1`) renders the body under a render context holding:
  - `loop.iteration`: `start + k * step` when iterating over a count, `k` over frames;
  - `loop.count`: `n`, the number of iterations the loop defines;
  - `loop.t`: `k / (n - 1)`, 0 for a single iteration;
  - `loop.frame`: the frame the iteration renders the body at.
- The same values are set as `loops.<name>.*`, `name` being the LoopBegin's `name`. A nested
  loop sets its own `loop.*` and keeps the outer loop's `loops.<name>.*`, so a body reads its
  own loop as `loop.*` and any enclosing one by name.
- Knob drivers in the body read these variables; body nodes whose knobs read none keep their
  context key and render once for every iteration at the same frame, sharing the cache with
  ordinary renders of that frame.

## Drivers

- A knob of a body node is driven from a loop from its right-click menu, Drive from loop, which
  is greyed out for a node outside any loop. The driver reads `loop.t` from the knob's default
  (usually its identity: Blur `size` 0, Grade `gain` 1) to its current value; each dimension
  gets its own driver, and Remove loop driver takes them off. Every edit is undoable.
- A driven knob shows a driver row in place of its fields: the dimension (All when every
  dimension is driven), the variable, start, end and easing, then the values the knob takes.
  The variable is `loop.t`, `loop.iteration`, `loop.count`, `loop.frame` or the same of an
  enclosing loop as `loops.<name>.*`. Easing is Linear (which extrapolates outside 0 to 1),
  Smooth, or Curve, edited from the row's curve button; Smooth and Curve clamp the variable
  to 0 to 1.
- Start and end animate like any parameter: right-click either, in the row or the table, to Set
  Key, Remove Key or Remove animation. Once keyed, editing the value keys it at the current
  frame, and the driven value at each frame goes between start and end at that frame, so the
  node's output varies with the frame.
- A driver whose variable no loop around the node sets, after the node was moved out of the
  loop or the loop renamed, keeps its value and shows a warning on the knob.
- The `drivenKnobs` table lists every driven dimension of the body's nodes, one row each,
  with the same variable, start, end and easing; editing a row edits the driver, and the knob's
  own row follows. The Value column shows the value at `singlePassIteration`.
- Driven and expression values outside a render, in the panel, the viewer's overlays or a
  script, are those at `singlePassIteration` of every loop enclosing the node, whatever the
  `mode`: there is no separate preview control.
- Performance: a driver is evaluated natively for each iteration and never takes Python's
  lock. A Python expression reading `loop.*` or `loops.*` is evaluated under the lock, so the
  iterations' expressions run one at a time across all render threads; prefer a driver when
  the knob is a straight ramp over the loop.

## Expressions

- A parameter expression in the body reads its loop as `loop.iteration`, `loop.count`,
  `loop.t` and `loop.frame`, and an enclosing loop as `loops.<name>.iteration` and so on, for
  example `2 + 8 * loop.t` or `loops.outer.t * loop.t`. The variables an expression reads are found when it is set: the
  body re-renders per iteration only for what it reads.
- Outside its loop a variable is None. In an expression `loop`, `loops` and `rfv` always
  name these variables, over a node of the same name.

## Block display

- The node graph draws a hull round each loop's LoopBegin, LoopEnd and the nodes on the paths
  between them, in a colour derived from the loop's `name`, with a wedge below the LoopBegin
  and above the LoopEnd. A nested loop's hull lies inside its enclosing loop's. Selecting
  either end of a loop draws its hull's outline solid and rings both ends. A node feeding the
  body from the side is part of the loop without lying inside the hull.

## Iterations

- `iterateOver` Count: `iterations` iterations, all at the current frame. 0 iterations
  outputs black with a warning.
- `iterateOver` Frames: one iteration per frame from the current frame plus `frameStart`
  to the current frame plus `frameEnd`, every `frameStep` frames (default -2 to 2 by 1),
  the body rendered at that frame. The body's frames are reported as the frames the LoopEnd
  needs. `frameEnd` before `frameStart` outputs black with a warning.
- More than `maxIterations` (default 1024, at most 4096) is an error rather than a
  truncated loop.

## Modes

- `mode` Accumulate folds the iterations with `operation`, per pixel and channel, pixels
  outside an iteration's region of definition counting as 0:
  - mean, sum, min, max;
  - over: each iteration over the ones before it; under: each iteration under them.
- The fold runs from iteration 0 to the last; `reverseOrder` runs it from the last to 0, so
  with over the first iteration ends on top. Mean, sum, min and max give the same result in
  either order, up to rounding.
- `mode` Single Pass outputs iteration `singlePassIteration` (counted from 0) alone,
  unfolded, with the variables it has in the whole loop: its `loop.t` is
  `singlePassIteration / (n - 1)`. Past the last iteration is an error. `operation`,
  `reverseOrder` and `stopCondition` do not apply.

## Stop condition

- `stopCondition`: a Python expression evaluated before each iteration `k`, in order, in the
  environment of a parameter expression on the LoopEnd: bare node names of its group
  (`Grade1.multiply.get()`), `thisNode`, `app` and the project's globals, `frame` (the current
  frame) and `rfv`. `rfv.<name>` reads the render context variables the LoopEnd receives from
  the Sets below it, at the evaluated frame and view, falling back to the project's variables.
  `loop.iteration`, `loop.count`, `loop.t` and `loop.frame` are those of the iteration, and
  `loops.<name>` is the same under the loop's name. The loop ends before the first iteration it
  is true for: only the iterations before it are folded, and mean divides by their number.
  Empty runs every iteration.
- It decides which iterations render, once per render and frame, and is never evaluated per
  pixel. It may read parameters; it cannot read the body's pixels. The
  variables of the loops enclosing this one are read as `loops.<name>.*`, at the iteration
  the enclosing loop is rendering.
- Ending before the first iteration outputs black with a warning. An exception, a syntax
  error among them, ends the loop at the iteration it was raised for and shows the error on
  the LoopEnd; the iterations before it still render.
- The parameters, `rfv` variables and project variables the stop condition reads are part of the LoopEnd's
  hash, as for an expression: editing one re-renders it. Its output is cached per frame, since
  the condition may read the frame. A node created after the condition was typed is only
  tracked once the condition is edited or the project reloaded.

## Limits

- Images only: a deep or scene input is refused at connection time.
- The region of definition is the union of the body's over the iterations, each at its
  frame.
- Metadata stays at its base values: the format, layers and other metadata are the body's
  outside the loop, not per iteration. A body whose format or layers change with a loop
  variable is folded as if they did not.
