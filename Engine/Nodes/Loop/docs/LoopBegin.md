# LoopBegin

Tolerance: none recorded.

- `LoopBegin`: marks the start of a loop body, which ends at the LoopEnd below it. The image
  passes through unchanged. See [LoopEnd.md](LoopEnd.md) for the loop variables, modes and
  limits.
- `name`: how a loop nested in this one sees its variables, as `loops.<name>.iteration`,
  `loops.<name>.count`, `loops.<name>.t` and `loops.<name>.frame`. An identifier, unique
  within the group; a new or pasted LoopBegin takes the first free `loopN`. An invalid or
  duplicate name is an error on the LoopBegin and its LoopEnd, whose output is then black.
- A LoopBegin pairs with exactly one LoopEnd: one reached from several LoopEnds is an error
  on each of them.
