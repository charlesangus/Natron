# LoopBegin

Tolerance: none recorded.

- `LoopBegin`: marks a start of a loop body, which ends at the LoopEnd below it. Several
  LoopBegins may start the body of one LoopEnd, each bringing in its own input. The image
  passes through unchanged, except under a LoopEnd in Feedback mode with `feedback` on: from
  the second iteration on, it outputs the previous iteration's output of the body, converted
  to its input's layer and bit depth. With the LoopEnd's `crossFrame` on and `feedback` on,
  frames after `startFrame` output the LoopEnd's output at the frame before. See [LoopEnd.md](LoopEnd.md) for the loop variables,
  modes and limits.
- `feedback`: on by default. Off, the LoopBegin passes its input through at every iteration
  of a Feedback loop and at every frame of a Cross Frame loop. Ignored in Accumulate mode
  without `crossFrame`.
- The loop's name is the LoopEnd's `name`.
- A LoopBegin pairs with exactly one LoopEnd: one reached from several LoopEnds is an error
  on each of them, as is a LoopBegin fed by another LoopBegin of its own loop.
