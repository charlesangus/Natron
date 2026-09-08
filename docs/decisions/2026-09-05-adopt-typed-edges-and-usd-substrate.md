# 2026-09-05 — Adopt typed graph edges and USD as the 3D substrate

Deep compositing and the 3D system are built on one generalization — typed
payloads (`image | deep | scene`) flowing along kind-checked edges, with images
as the default kind so every existing node and OFX plugin is untouched — rather
than two ad-hoc side-channels. For 3D, the scene substrate **is** USD (model A:
per-node authored `SdfLayer`s flowing as a `ScenePayload` layer stack, composed
only at consumers, rendered via Hydra/Storm), not a native scene graph with
translation layers (model B). Rationale: Natron has no existing 3D data model
and no simulation ambitions, so model B's costs — perpetual schema authorship,
a second interchange system, a third viewport system — buy nothing here, while
USD supplies schemas, interchange, viewport, and a renderer ecosystem as pinned
dependencies. Node code talks to USD only through the thin `SceneOps` seam so
pxr types stay out of `Engine/` core headers.

Full analysis, mitigations, and the phased milestone plan:
`PLAN/DESIGN/2026-09-05-deep-and-3d-native-extensions.md` (elaborated as
M17–M21). Diverges from upstream project-file compatibility for projects using
deep/scene edges — accepted for this fork.
