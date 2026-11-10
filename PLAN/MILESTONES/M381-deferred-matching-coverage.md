# M381 - Deferred Matching Coverage

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

M331 - Deferred Pattern Matching moved some pattern nodes onto load sets. The rest still match against fully loaded stages. This milestone converts them.

- Pattern nodes not yet on load sets: USDSetAttribute, USDEditPrim, USDVariant, USDProjectUV.
- Query-stage borrowers that still load everything: USDEditTransform's `transformsPrim` and `centrePivot` (Replace mode with stale matches), the SceneOps merge/graft `readInput` and `rootHoldsGprim`, USDRender, USDProject3D, USDRenderSettings, and USDRead's metrics correction (only when Match Up Axis or Units is on).

The open design question is whose load set applies to each borrower: the viewer's, or that of a panel showing the node.

Blocked on: M331 - Deferred Pattern Matching landing, and a decision on whose load set applies to each borrower (viewer vs. panel). M332 - Scene Graph Points may also shape the borrower list.

Acceptance sketch:
- Every node listed matches on a load set. A heavy-scene test shows that no fully loaded stage is built on edit for any of them.
- Results with a partial load set match results on a fully loaded stage for each converted node.
- Every borrower has a documented load-set owner.
