# M315 - Direct Texel Painting

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

A second paint model for TexturePaint: raycast splat painting straight into UV space, alongside the Mari-style paint buffer M53 - Projected Texture Painting ships first (user, 2026-11-02: "both, buffer first").

Blocked on: M53 - Projected Texture Painting's paint-buffer model, stroke storage and undo, which this model must share.

Acceptance sketch:
- A Paint Model switch picks buffer or direct texel painting.
- Direct mode splats each dab into the texture through mesh raycasts, with no projection bake step.
- Strokes from both models persist, replay and undo the same way.
