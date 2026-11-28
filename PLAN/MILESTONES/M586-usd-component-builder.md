# M586 - USD Component Builder

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Asset publishing: a node set that writes a USD component, meaning a payload, geometry variants, a material layer, a class prim and a standard folder layout. Design reference: Solaris Component Builder (`.local/docs/houdini/solaris/component_builder.md`, `nodes/lop/component*.md`).

Member of F43 - USD Asset Pipeline. The component's material layer is a look layer from M582 - Look Layers, written through F27 - USD Layer Authoring's layer control.

Blocked on: the user's approval to proceed (user, 2026-11-24: every stub from the feature-gap survey waits for a go-ahead); then M582 - Look Layers and F27 - USD Layer Authoring landing.

Acceptance sketch:
- An asset built from native geometry and a material publishes as a component folder that USDReference loads with its variants.
