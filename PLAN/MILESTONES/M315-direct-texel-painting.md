# M315 - Direct Texel Painting

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

A second paint model for TexturePaint: raycast splat painting straight into UV space, alongside the Mari-style paint buffer M53 - Projected Texture Painting ships first (user, 2026-11-02: "both, buffer first").

Blocked on: M53 - Projected Texture Painting's paint-buffer model, stroke storage and undo, which this model must share.

Acceptance sketch:
- A Paint Model switch picks buffer or direct texel painting.
- Direct mode splats each dab into the texture through mesh raycasts, with no projection bake step.
- Strokes from both models persist, replay and undo the same way.
- Built on `USDTexturePaint` (M53 - Projected Texture Painting's `TexturePaint`, renamed). If it lands after M427 - Native Geometry Texture Painting, it lives in the shared texture-paint base and works on `GeoTexturePaint` too (user, 2026-11-14).

## Decisions
- 2026-11-26 (PM): waits for M312.P1.T1 (shared USDTexturePaint files); elaboration is in hand from the 2026-11-26 consultant pass and is written when M312.P1.T1 lands.
