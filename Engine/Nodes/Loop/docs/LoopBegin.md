# LoopBegin

Tolerance: none recorded.

- `LoopBegin`: marks the start of a loop body. The image passes through unchanged.
- `name`: how a loop nested in this one sees its variables, as `loops.<name>.*`. An
  identifier, unique within the group; a new or pasted LoopBegin takes the first free
  `loopN`. An invalid or duplicate name is an error on the LoopBegin and its LoopEnd.
