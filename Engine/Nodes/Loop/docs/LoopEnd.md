# LoopEnd

Tolerance: none recorded.

- `LoopEnd`: renders its input once per iteration and folds the iterations together. The
  body starts at the LoopBegin found walking up from the input; none, several, or a
  LoopBegin also reached from another LoopEnd is an error, and the output is black.
- Each iteration `k` of `iterations` renders under a render context holding
  `loop.iteration` (`start + k * step`), `loop.count` (`iterations`), `loop.t`
  (`k / (iterations - 1)`, 0 for a single iteration) and `loop.frame` (the current
  frame), and the same values as `loops.<name>.*`, `name` being the LoopBegin's. A nested
  loop overrides `loop.*` and keeps the outer loop's `loops.<name>.*`.
- Body nodes whose knobs read no loop variable keep their context key and render once.
- `mode`: Accumulate. `operation`: mean, sum, min or max, per pixel and channel, with
  pixels outside an iteration's region of definition counted as 0.
- `iterations` 0 outputs black with a warning. More than `maxIterations` (default 1024, at
  most 4096) is an error rather than a truncated loop.
- Images only: a deep or scene input is refused at connection time.
- The region of definition is the union of the body's over the iterations.
