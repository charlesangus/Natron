# M582 - Look Layers

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Lookdev-to-lighting handoff in plain USD: export an asset's materials and their bindings as a layer of their own, through F27 - USD Layer Authoring's layer control. A Look node then applies such a layer to another stage, remapping prim paths where the asset sits elsewhere. Design references: Katana Look Files / LookFileManager and Solaris Component Material (`.local/solaris-gaffer-katana-gap-survey-2026-11-23.md`).

Member of F43 - USD Asset Pipeline.

Blocked on: the user's approval to proceed (user, 2026-11-24: every stub from the feature-gap survey waits for a go-ahead); then F27 - USD Layer Authoring landing.

Acceptance sketch:
- A lookdev script writes a layer holding only materials and bindings for an asset.
- A lighting script applies that layer to a shot stage where the asset sits at a different path, and every binding resolves.
