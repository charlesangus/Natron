# M583 - Wedge And Task Dispatch

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

An ordered task graph of Write and USDRender nodes, Python commands and frame masks, run by a local dispatcher in the background, plus a Wedge that steps a knob's value per task. Shot variables were declined (user, 2026-11-24), so a wedge iterates knob values directly. Design reference: Gaffer's Dispatch framework (`.local/docs/gaffer/1.7.5.0/Reference/NodeReference/GafferDispatch/`).

Blocked on: the user's approval to proceed (user, 2026-11-24: every stub from the feature-gap survey waits for a go-ahead).

Acceptance sketch:
- A graph with two Writes, where one Write depends on the other, renders in dependency order in the background while the UI stays responsive.
- A Wedge over a knob writes one output per value, each named by that value.
