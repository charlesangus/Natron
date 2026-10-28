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
- `mode` Feedback: see [Feedback](#feedback).
- `singlePass` outputs iteration `singlePassIteration` (counted from 0) alone, unfolded,
  with the variables it has in the whole loop: its `loop.t` is
  `singlePassIteration / (n - 1)`. Under Feedback it is that iteration's output, after the
  iterations before it. Past the last iteration is an error. `operation`, `reverseOrder` and
  `stopCondition` do not apply.

## Feedback

- `mode` Feedback chains the iterations instead of folding them: iteration 0 renders the body
  on the loop's input, and each later iteration renders it on the output of the one before,
  which the LoopBegin outputs in place of its input. The LoopEnd outputs the last iteration.
  Five iterations of a 1 px Erode give a 5 px Erode; ten 1 px translates, a 10 px one.
- The variables are those of Accumulate. A body that reads none of them still renders once
  per iteration: each iteration reads a different image.
- `stopCondition` applies: the output is the last iteration before the one it is true for.
  `operation` and `reverseOrder` do not apply.
- The LoopBegin keeps its input's format, layers and bit depth. A body whose output has other
  layers or components is converted to them on the way back to the LoopBegin, with a warning
  on the LoopBegin.
- Region of interest: each iteration asks the one before it for what the body needs, so the
  region asked of the loop's input grows by every iteration. n iterations of a body reading r
  pixels around each pixel ask the input for n·r pixels around the output region, as one
  n·r filter would. The region of definition follows the body from iteration to iteration.
- Memory: an iteration's output is held only until the next iteration has read it, so the
  memory in use does not grow with the number of iterations, within the render's bytes
  budget. The render time does: every iteration renders the whole body. Nodes inside the
  body cache per iteration; a cached render of the loop is reused until the body, a node
  feeding it, a parameter one of its expressions reads, or a LoopEnd iteration parameter
  changes.
- Frames: with `iterateOver` Frames, iteration k renders the body at its own frame and reads
  the previous iteration's output, rendered at the frame before it by `frameStep`. A body
  adding the frame's image onto the LoopBegin's leaves a trail of the frames from
  `frameStart` to `frameEnd`. A time offset in the body offsets each iteration again: a node
  reading the LoopBegin one frame back, over three iterations, reads the loop's input three
  frames back. Feedback from one output frame to the next is `crossFrame`: see
  [Cross Frame](#cross-frame).

## Cross Frame

- `crossFrame` feeds each output frame into the next: after `startFrame`, the LoopBegin outputs
  this LoopEnd's output at the frame before instead of its input, under Accumulate at every
  iteration and under Feedback at the first. At `startFrame` and before it, the LoopBegin
  outputs its input. A trail or a simulation builds up from `startFrame`, the iterations of a
  frame being its substeps: four Feedback iterations make four substeps a frame.
- A frame needs every frame before it back to `startFrame`. Rendering frame f renders, in
  order, the frames from the latest one kept before f, or from `startFrame`, to f, on one
  thread; the progress panel shows a run of more than two frames, and cancelling it fails that
  render, keeping the frames rendered so far.
- The stored window is the LoopEnd's region of definition, unclipped: a frame is kept whole,
  beyond the format where it reaches past it, so what leaves the format comes back.
- Memory: each frame's output is kept in RAM under a budget, by default a quarter of what the
  image cache may use. Over the budget the least recently used frames go first and the
  checkpoints last: the frames at `startFrame + k * interval`, the interval a power of two
  chosen so the checkpoints of every series fit in a quarter of the budget. A frame no longer
  kept is rendered again from the checkpoint before it. Editing the loop, its body or anything
  upstream of it starts a new series and drops the frames kept for the old one.
- The body reads only the frame before. A body node that asks its LoopBegin for another frame,
  by changing `loops.<name>.frame`, would wait for a frame its own render holds up: the render
  fails at once instead, with an error on the LoopEnd naming `loops.<name>.frame`.
- Each view, proxy level and render context renders its own series.

## Bake

- `bake` renders `startFrame` to `bakeEnd` in order on a background thread and writes each
  frame to `cacheFile`, an EXR sequence with its frame number written `####` or `%04d`
  (`sim.####.exr`). The progress panel shows the bake and cancels it; the frames written by
  then stay, and a cancelled bake posts no message. A bake that fails, with Cross Frame off,
  `bakeEnd` before `startFrame`, an empty `cacheFile` or one without a frame number, posts an
  error on the LoopEnd. While a bake runs, the LoopEnd's own renders compute rather than load.
- Each file is a float EXR of the LoopEnd's output layer whose data window is the frame's
  region of definition, unclipped: it can extend outside the format. It carries
  `natron:loopHash`, the loop's simulation hash when the frame rendered: the LoopEnd's
  parameters, the body and everything upstream. The bake renders view 0 at full scale, in the
  root render context.
- `loadFromCache` reads a baked frame instead of rendering it, ahead of the driver: frames up to
  the last one baked render none of the body. A frame past the bake continues from the last
  frame baked before it. Frames load for view 0 in the root render context; other views and
  contexts render.
- A frame whose `natron:loopHash` differs from the loop's current hash, after an edit to the
  loop or upstream of it, still loads, as baked, and the LoopEnd shows a persistent warning
  until the frames read match again; bake again to update them. A file that cannot be read is
  rendered instead, with a warning.
- `clearCache` forgets the frames kept in RAM; the files stay. A bake, and turning
  `loadFromCache` on or off, starts a new series, so frames rendered before it are not mixed
  with frames read from the files.
- An upstream node whose hash changes between sessions, such as one holding a per-session
  salt, warns on every load after a relaunch.

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
  frame; under Feedback, the last iteration's.
- Metadata stays at its base values: the format, layers and other metadata are the body's
  outside the loop, not per iteration. A body whose format or layers change with a loop
  variable is folded as if they did not.
