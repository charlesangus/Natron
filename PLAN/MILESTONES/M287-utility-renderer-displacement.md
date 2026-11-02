# M287 - Utility Renderer Displacement

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Texture-driven displacement in Natron Utility: UsdPreviewSurface's `displacement` input (and
a scalar or vector displacement texture) applied to refined subdivision meshes along the limit
normal, with displaced bounds, recomputed normals and motion-blur support.

Blocked on: M289 - Utility Adaptive Subdivision's outcome — whether displacement dices through
screen-space adaptive refinement or the uniform level from M284 - Utility Renderer
Subdivision — and the user's go-ahead (displacement was "possibly", 2026-11-01).

Acceptance sketch:
- A displacement-mapped subdivided plane renders the displaced silhouette on both backends.
- Displacement bounds are padded so no displaced geometry is clipped.
- Deforming displaced meshes motion-blur correctly.
