# M586 - USD Component Builder

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Asset publishing: a node set that writes a USD component, meaning a payload, geometry variants, a material layer, a class prim and a standard folder layout. Design reference: Solaris Component Builder (`.local/docs/houdini/solaris/component_builder.md`, `nodes/lop/component*.md`).

Blocked on: the user's approval to proceed (user, 2026-11-24: every stub from the feature-gap survey waits for a go-ahead).

Acceptance sketch:
- An asset built from native geometry and a material publishes as a component folder that USDReference loads with its variants.
