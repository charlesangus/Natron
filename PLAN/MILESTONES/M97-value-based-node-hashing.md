# M97 - Value-Based Node Hashing

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Natron's node hash counts edits rather than hashing values: `knobsAge` increments on every change, so undoing a change never returns the hash to its old value, and the hash also mixes in the script name and the project's creation time. That makes it useless for anything that must recognise "the same output again" across undo, sessions or machines — the Write's hash-gated read back (Nuke-style "read file only if it matches the node") was the first casualty. This milestone replaces it with a hash of the values that determine a node's output (plug-in ID/version, input topology, knob values/curves/expressions at the frame, file identity for readers), usable by the cache, the renderer and the Write's read back.

Blocked on: the user's design call on the scope — whether the value hash replaces the node hash everywhere (cache keys, render invalidation) or sits beside it — raised 2026-10-11 during M76 - Native Write UAT.

Prior art: branch `m76/hash-gated-readback-wip` (`f1fbe003a`) has a working value-based content hash (`EffectInstance::computeContentHash`, `Engine/EffectInstanceContentHash.cpp`) and the Write's hash-gated, EXR-only read back on top of it (the hash stored as `natron:outputHash` in the EXR header), with tests; its Xvfb check passed the full undo-restores-the-match flow.

Acceptance sketch:
- Undoing a change returns a node's hash to its previous value; renaming a node or reopening the project in another session doesn't change it.
- The Write reads back an EXR frame only when the hash stored in the file matches the node's current hash, and shows its input otherwise; read back is offered only for EXR.
- Cache hits survive undo/redo of a change.
