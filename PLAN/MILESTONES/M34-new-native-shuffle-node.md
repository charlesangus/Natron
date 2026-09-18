# Milestone 34: New native Shuffle node

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

The existing Shuffle node's UI/semantics are counter-intuitive and reportedly
broken. Design and ship a new native Shuffle node with clearer
channel-mapping semantics, replacing (or living alongside, then replacing)
the current one.

Blocked on: not yet prioritized — captured as backlog (user request
2026-09-18); needs a design pass on the new node's UI/semantics before
elaboration.

Acceptance sketch:
- A native Shuffle node ships with clear, testable channel-mapping
  semantics.
- Existing projects using the old Shuffle node still load (migration or
  compatibility path defined).
