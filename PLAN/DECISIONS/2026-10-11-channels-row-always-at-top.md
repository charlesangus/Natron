# Channels row is always at the top of a node's panel

- **Date:** 2026-10-11
- **Decided by:** user
- **Context:** M76 - Native Write's panel layout (P2.T16) planned the Write's channel-set row after Frame Increment. The host creates the row in `Node::createLayerKnob` after the effect's own knobs, at index 0 of the main page, so moving it would need a per-node placement hook.

**Decision:** a node's Channels (layer/channel selector) row always goes at the top of its main page, for every node, with no per-node placement. Node panels lay out their own knobs below it.
