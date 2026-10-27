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

- `stopCondition`: a Python expression evaluated before each iteration `k`, in order, with
  `loop.iteration`, `loop.count`, `loop.t` and `loop.frame` of that iteration and `frame`
  (the current frame) set, and the project's globals (`app1`, ...) visible. The loop ends
  before the first iteration it is true for: only the iterations before it are folded, and
  mean divides by their number. Empty runs every iteration.
- It decides which iterations render, once per render and frame, and is never evaluated per
  pixel. It may read parameters; it cannot read the body's pixels.
- Ending before the first iteration outputs black with a warning. An exception, a syntax
  error among them, ends the loop at the iteration it was raised for and shows the error on
  the LoopEnd; the iterations before it still render.
- A LoopEnd with a stop condition does not keep its own output in the cache, since what the
  expression reads is not part of the node's hash.

## Limits

- Images only: a deep or scene input is refused at connection time.
- The region of definition is the union of the body's over the iterations, each at its
  frame.
- Metadata stays at its base values: the format, layers and other metadata are the body's
  outside the loop, not per iteration. A body whose format or layers change with a loop
  variable is folded as if they did not.
