# Milestone 35: Remove implicit output-plane shuffling from non-Shuffle nodes

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Nodes other than Shuffle currently expose an "output plane" selector that
effectively lets them shuffle channels. Drop that from all non-Shuffle
nodes so they process channels in-place, and point users to the Shuffle
node for actual shuffling.

Blocked on: M34 — needs the new native Shuffle node shipped as the
supported replacement path before removing the escape hatch elsewhere.

Acceptance sketch:
- Non-Shuffle nodes no longer expose an output-plane/channel-shuffle
  selector.
- Channel remapping is only available via the Shuffle node.
