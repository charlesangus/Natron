# Milestone 35: ~~Remove implicit output-plane shuffling from non-Shuffle nodes~~ (cancelled — folded into M38)

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

## Decisions

- 2026-09-19 — **Kept behind M34** (user decision): the 2026-09-19 reorder had placed M35 ahead of M34, but the stub's block — the new native Shuffle node must exist as the supported replacement before the escape hatch is removed elsewhere — still holds. Board row moved to directly after M34.
- 2026-09-19 — **Folded into M38** (user decision, later the same day): the removal of the output-layer/input-layer selectors is one half of M38's new layer/channel widget, so it ships there. Row cancelled.
