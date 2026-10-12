# M113 - Native Node Rewrites Continued

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Continue M67 - Native Core Nodes and M73 - Native Core Nodes Wrap-Up: rewrite more of the OpenFX nodes users rely on as native nodes. The first entries are **IDistort** and **STMap**. They use native layer/channel knobs, so U and V come from any layer through the normal layer/channel picker. The openfx-misc "Alpha Channel" UV-coverage multiply and the "Unpremult UV" option are **dropped** (user, 2026-10-12). Further nodes are added here as the user names them.

Blocked on: M56 - Visual And Menu Polish merging, since it changes the IDistort/STMap take-over layout this milestone replaces; and the user naming any further nodes beyond IDistort and STMap.

Acceptance sketch:
- Native IDistort and STMap replace the OFX versions, and match their output on fixtures (UV offset and scale, filtering, wrap modes), apart from the dropped alpha features.
- U and V come from any layer of the UV input, through the standard layer/channel knob.
- No OFX IDistort or STMap remains in the default plugin set.

## Decisions

- 2026-10-12 — **Created from M69 - Minor Cleanup (user):** instead of giving the OFX IDistort and STMap a UV-layer dropdown (the cancelled M69.P9.T1), rewrite them as native nodes, drop the alpha channel feature, and use this milestone for further native-node rewrites.
