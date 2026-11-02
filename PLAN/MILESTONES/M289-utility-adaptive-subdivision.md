# M289 - Utility Adaptive Subdivision

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Screen-space adaptive dicing for Natron Utility's subdivision surfaces, as Karma and
RenderMan do: refine each patch to a target edge length in pixels (with an off-screen and
motion-aware rate), instead of M284 - Utility Renderer Subdivision's uniform level. It is the
natural base for M287 - Utility Renderer Displacement.

Blocked on: M284 - Utility Renderer Subdivision being done — its stencil cache and refined
mesh path are what this replaces or extends, and the choice between OpenSubdiv feature-adaptive
patches and a custom dicer depends on how that code turns out.

Acceptance sketch:
- A `Dicing Rate` setting (pixels per edge) replaces the level for meshes that opt in.
- Close geometry is finer and distant geometry coarser, with no cracks between patches.
- Render time scales with screen coverage, not cage size × level.
